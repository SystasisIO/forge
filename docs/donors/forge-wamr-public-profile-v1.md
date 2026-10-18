# WAMR Bounded Deterministic Public Profile

Decision date: 2026-10-16. Status: bounded/single-result policy and the scoped
parser/raw-entry breaks below accepted; other
numeric constants remain PROPOSAL. Private and focused public results below
are scoped historical checkpoints. Current usage-1 allocator integration and
fresh full qualification are pending. This is not a release or a claim of
complete runtime qualification.

## Approved Requirement Change

The maintainer explicitly chose a bounded deterministic WAMR profile instead of
preserving the entire historical EOS-derived capacity range. In particular,
the 65,536-i32-local fixture remains an expected rejection. Do not widen WAMR's
loader, fast-interpreter offsets or AOT representation to preserve unused
extremes. This changes the supported input domain deliberately, not silently.

Preserve consumer APIs except for the explicitly scoped raw-entry break below,
required donor contracts, determinism, checked memory,
typed host failures, cancellation and invocation isolation within the admitted
profile. No fallback to EOS, disabling checks, removing rejection fixtures or
using a test-only host adapter as the public backend is authorized.

The [API map](forge-wamr-api-map-v1.md),
[iteration acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md#acceptance-matrix)
and [migration note](../releases/unreleased-wamr-bounded-profile.md) are current
requirements. Older qualification reports and the
[baseline manifest](forge-wamr-baseline-v1.json) are immutable observations,
including their then-current full-capacity requirement.

## Approved Parser And Entry Scope

The human maintainer explicitly approved retiring these three malformed-WASM
permissions in the replacement profile on 2026-10-16:

| Historical option | Current profile requirement |
| --- | --- |
| `allow_code_after_function_end` | Unsupported/deprecated permission; reject instructions after the function end, including when the historical permission is enabled |
| `allow_invalid_empty_local_set` | Unsupported/deprecated permission; reject an invalid value type in an empty locals group, including when the permission is enabled |
| `allow_u32_limits_flags` | Unsupported/deprecated permission; reject reserved memory/table flags, including when the permission is enabled |

This is an explicit compatibility decision, not a parser fix or an ignored
failing test. Preserve historical donor fixtures/manifests and result records;
new profile regression tests must expect rejection, with valid controls and
equal interpreter/validator/compiler policy. No new parser or upstream widening
is authorized. `allow_zero_blocktype` is NOT covered: its admission-policy gap
remains open. Old parser failure counts are historical observations, not fresh
green results after this decision.

The maintainer subsequently also approved the narrow old OC machine-entry API
break: remove `artifact::entry()` and `entry_function`. The AOT owner and
coordinator confirmed source removal and migration through the existing public executor,
not a raw-entry adapter or an always-throw placeholder. Exact confirmed
declarations and migration are named in the
[release note](../releases/unreleased-wamr-bounded-profile.md#raw-oc-entry-api-break).
Preserve `compiled_module`, host APIs, executor and session/cache; this is not
permission for broader removals. The coordinator's donor trace found internal
Spring `apply_func` pointer calls, not a Forge raw-entry API consumer. WAMR's own
ABI supplies the generic native-call mechanism. Implementation, consumer and
runtime qualification remain separate gates.

## Common Admission Contract

The [2026-10-18 follow-up](forge-wamr-dev-integration-20261018.md) records the
modern single-memory index encodings and polymorphic `br_table` typing used by
the existing engine, with fresh positive and negative public tests. The bounded
numeric feature set does not imply the old MVP oracle's exact grammar or full
support for every current Core proposal. Historical oracle differences remain
visible and do not authorize hiding an unrelated compatibility failure.

Interpreter admission, validation-only use, compiler source admission and AOT
artifact admission must refer to the same versioned profile. A supported
architecture or interpreter build mode must not change the admitted module set.
Use the upstream-validated module and shared Forge observations, not a second
parser or copied SDK registry. WAMR types remain private.

The agreed bridge is owned by `interpret`: `prepared_module` handles original
source admission and has a source-plus-native overload in the current source,
with `runtime_context` naming `prepared_module::context` for both modes.
Original source admission must precede native use. The presence of this overload
is not complete public AOT, consumer or installed-runtime qualification. The
existing optimizing session, cache and lifecycle remain with their current owner.

Caller budgets may narrow the profile. Unsupported or unrepresentable settings
must be rejected explicitly, not ignored, truncated or silently clamped.
Distinguish malformed WASM, profile/caller-budget refusal, logical exhaustion,
cancellation and local resource failure. Diagnostics are not a production
classification protocol. Reject before guest initialization and effects.

Whole initialization includes an imported start function and conventional
`__post_instantiate` hooks, not only a defined WASM start section. Their ordering,
single execution, pre-cancel behavior and typed host failure propagation are
required integration cases. The [reviewed initialization fix](forge-wamr-fork-v1.md#imported-initialization-follow-up)
landed at `65f974b` and is included in the current `66e6be6` pin; complete
public invocation behavior remains a separate gate. Capacity and selected real
contract-action probes do not close it. Structured engine errors and a neutral
public contract now exist; complete public classification/resource transport
still needs fresh evidence. A private `std::runtime_error` is not that contract.

The public interpreter owner owns the shared constants in
[`capacity_profile`](../../libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret/capacity_profile.cppm).
The table records current decisions, including the revised page-cap proposal;
owner implementation and fresh tests are separate gates, not implied by this table.
The P1 single-result rule is an accepted admission decision. The other numeric
values remain PROPOSAL pending complete public/physical qualification, not
inferred supported maxima or new accepted product defaults:

| Field | v1 value or revised proposal | Disposition |
| --- | --- | --- |
| `version` | 1 | Current contract identifier, not qualification status |
| `max_module_bytes` | 16,777,216 (16 MiB) | PROPOSAL |
| `max_function_storage_bytes` | 32,768 (32 KiB), the maximum of type-signature parameter/result bytes and per-function parameter/local/logical-peak-operand bytes | PROPOSAL |
| `max_function_results` | 1 | Accepted P1 multi-result rejection; complete cross-mode qualification pending |
| `max_call_depth` | 1024 | PROPOSAL |
| `max_control_depth` | 1024 | PROPOSAL |
| `max_pages` | 65,535 WASM pages = 4,294,901,760 bytes (4 GiB minus 64 KiB) | Revised PROPOSAL derived from `UINT32_MAX / 65536`; fresh boundary qualification pending |
| `max_instance_metadata_bytes` | 2,147,483,647 (`INT32_MAX`) prospective AOT instance metadata bytes, including extra-state extent | Representation-derived PROPOSAL; common source check present, fresh full qualification pending |

The previous 65,536-page figure was an unqualified proposal, now withdrawn,
not a supported historical maximum. Review found that exactly 65,536 pages
produce 4 GiB of bytes, which narrows to zero in the existing u32 usage-callback
size ABI. The revised cap is `floor((2^32 - 1) / 65536) = 65535`; no WAMR
counter or callback widening is approved. Over-cap initial memory and unsupported
explicit configuration must be rejected before that narrowing, allocation or
guest effects, using typed profile/configuration refusal. This is not an
ambiguous local allocation failure and must not be reported as one.

The common source predicate checks INITIAL logical pages, not the declared
maximum. A valid initial memory with declared maximum 65,536 or no explicit
maximum stays admissible, with growth bounded by 65,535 and the caller/module
policy. Explicit configuration caps 65,536 through `UINT32_MAX - 1` are refused;
the existing `UINT32_MAX` sentinel intersects the common profile rather than
authorizing a 4 GiB allocation. These agreed semantics require fresh tests.

Valid memory with zero initial pages is distinct from an overflowing nonzero
request: preserve its initialization, first growth and zero-fill behavior.
Boundary admission tests must not instantiate 4 GiB to infer the cap. An
admitted source's actual allocation can still be locally unavailable; such a
physical failure remains operational, not a profile or canonical guest result.

The source-owned metadata guard now projects the prospective AOT instance on
the supported 64-bit ABI using actual upstream layout: header offset plus
memory-instance extents and numeric global bytes, aligned to eight bytes,
then each `wasm_table_instance_size` with growing maximum or fixed initial
table count, plus `sizeof(AOTModuleInstanceExtra)`. Common admission rejects
the total above `INT32_MAX` before instantiation/codegen. This is a native
signed-offset representation bound, not another linear-memory page limit or
proof of physical allocation feasibility. Exact/next-representable source-only
tests and compiler/runtime equality remain fresh qualification gates; do not
allocate a huge table to test them or duplicate public WAMR types.

Enforce `max_function_results` on every upstream-validated function type before
public result projection, including unused types and imports. Runtime preparation,
validation-only loading and compiler source admission share `check_profile`.
The public checkpoint tests a valid `() -> (i32, i32)` module and requires typed
profile rejection; it does not stand in for a fresh compiler/AOT gate. See the
[public compatibility record](../../tests/vm_wasm/WAMR_COMPATIBILITY.md).

The source of truth is this single Forge-owned contract, not a duplicate
constants table in the compiler. Its identity must reach admission and native
artifacts. Platform feasibility, peak allocations, fast-offset representability
and exact/next-value tests remain required; a field declaration does not prove
any of them. The following remain required before acceptance:

The current shared predicate is the maximum over every type's parameter plus
result cells and every function's parameter plus local plus logical peak operand
cells, multiplied by four bytes per cell. i64/f64 use two cells. Caller budget
flags select `1=params`, `2=locals`, `4=operands` (default `6`); those flags do not
disable or change this independent common predicate. Caller-selected function
bytes and physical runtime frame bytes are distinct quantities.

| Bound | Required accounting and evidence | State |
| --- | --- | --- |
| Parameters, locals, operand cells and results | Type-aware bytes/cells; combined offset representability in classic/fast/AOT; exact cap and next representable value | PROPOSAL; private 32 KiB/+4 source boundary passes; focused public classic case passes; every signature/offset shape and public AOT pending |
| Function bytes | Per-function accounting selected by existing flags, not a sum of unrelated module maxima; caller 8192/16384-byte boundaries | Diagnostic evidence available; public integration pending |
| Logical depth | Defined, indirect and guest-to-host calls plus guest initialization; configured 251 and 1024 boundaries and one extra call | Local public classic recursive fixture passes selected 8/16/32 KiB by 251/1024 cases; full initialization/import/indirect/AOT profile remains partial |
| Module/section/code/symbol/control limits | Bounded allocation before excessive loader work; existing option fallback and zero semantics | Partial private metrics; public integration pending |
| Linear memory | Proposed 65,535-page cap derived from u32 callback bytes; typed early initial/config refusal at 65,536, valid zero initial pages and bounded growth; no coalesced-page/integer-wrap bypass | Selected public initial/config/zero/growth and fresh compiler exact/excess cases pass; expanded private runtime matrix NOT_RUN, full all-mode physical/public qualification open |
| Function results | Every validated type admits at most one result; valid multi-result input rejected before projection, identically in source/validator/compiler paths | P1 policy accepted; focused public source/validator evidence, fresh compiler/full matrix pending |
| AOT artifact | Engine revision, profile, target/ABI, imports and compiler provenance checked before native loading | Pending |

## Neutral Failure Contract

The [public compatibility record](../../tests/vm_wasm/WAMR_COMPATIBILITY.md#neutral-trap-contract)
defines `exceptions::guest_trap : interpreter` with `guest_trap_kind kind`;
existing `catch(interpreter&)` remains valid. Reasons are `unknown`,
`unreachable`, `table_bounds`, `indirect_type`, `integer_divide_by_zero`,
`integer_overflow`, `invalid_conversion`, `logical_depth` and `auxiliary_stack`.
Memory bounds retain `exceptions::memory`. Physical native/operand stack and
local allocation failures are operational `execution_unavailable`, not logical
guest-depth traps. Unknown and auxiliary-stack reasons cannot become canonical
guest outcomes without an explicit supported rule; SDK execution fails closed.

Precedence is original host failure, cancellation, dedicated logical-depth flag,
then structured engine exception information. No message parsing is allowed.
The current fork supplies NONE/UNKNOWN/KNOWN information and the original engine
ID; the compatibility record's missing-getter discussion describes its old
`65f974b` checkpoint. New enum/getter presence and private usage-0 tests alone
do not qualify public interpreter/AOT mappings or remove recorded failures.

## Linear-Memory Ownership

The common [build profile](../../cmake/ForgeWamrProfile.cmake) now enables
`WAMR_BUILD_ALLOC_WITH_USAGE=1` for the approved existing-allocator plan.
Implementation is present; the allocator owner reports the common production
source frozen, not the full profile qualified. A fresh matching compiler and
full standalone/public matrices remain pending. One process-lifetime
dispatcher separates system runtime allocations from guest linear memory;
scoped owners cover initialization, calls, reentry, teardown and failure cleanup.
Public `prepared_module::context` must borrow the caller's existing
`wasm_allocator` exclusively for the instance lifetime. Its mapping is the sole
linear-memory storage: no second buffer, moving growth or public system-provider
fallback. Nested scopes restore their owner and parallel owners remain isolated.
Callback exceptions must not unwind through C.

The private engine owner reports a real system-backed provider for standalone
instances, scoped by that same dispatcher. It is not a fake allocator or public
allocator qualification. The coordinator has registered `engine_memory.cpp` and
the shared runtime-profile helper after the upstream include in standalone CMake,
removing its duplicate feature list. Source wiring is present, not build/test
evidence. Neither the provider report nor enabling the ABI flag proves actual
initialization/growth/lifetime behavior. Wait for the allocator/provider freeze.

Required public evidence includes zero pages/first grow, stable base, retained
data and zeroed new pages, caller/module limits, fresh reset, exclusive-borrow
refusal, nested/parallel ownership and cleanup after partial initialization,
host failure, cancellation and local resource failure. Fresh compiler/runtime
identities must agree on usage-1 ABI; do not override usage back to zero or
relabel old binaries to obtain a passing run.

## Physical Stack And Concurrency

A function-byte cap and logical call-depth counter do not prove physical stack
safety. Size the interpreter value stack from admitted parameters/locals/live
operands, per-frame overhead, initialization/host transitions and admitted depth,
using checked arithmetic. Bound memory across simultaneous invocations. Native
host stack and AOT live native frames need their own measured bounds and guards;
unused padded locals optimized away by LLVM are not a worst-case fixture.

The preserved 2026-10-16 runtime qualification found that a 32 MiB diagnostic
stack passed all 32 large-frame/depth combinations, while a 1 MiB interpreter
stack failed all 16 interpreter cases and recovered for shallow reuse. Neither
number is a production default, measured minimum or acceptable substitute for
coherent provisioning. Local allocation/stack failure must not be presented as
a deterministic contract result. Heavy live AOT frames remain an open gate.

## Current Scoped Public Checkpoint

The allocator owner froze production sources/leaf CMake at fork `66e6be6` and
common profile SHA-256
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`.
The coordinator independently verified the current XML: public execution
**27/27 cases, 244 assertions**, private system-memory/metadata **4/4 cases,
130 assertions**, and the four selected CTest targets **4/4 PASS**. The
[owning compatibility record](../../tests/vm_wasm/WAMR_COMPATIBILITY.md#current-allocator-and-shared-profile-freeze)
names the warm directory, logs and exact scope. This is local Release evidence,
not all-mode public AOT/physical-stack or installed/product acceptance.

This checkpoint covers actual caller allocator storage, zeroing and zero-page
growth, exclusive nested/parallel ownership, cleanup/reuse, physical failure
before subsequent host effects, initial/configuration page boundaries, adjacent
native-metadata source extents, structured getter transport and the three
approved malformed-permission rejection policies. It closes the recorded
old-pin invocation trap failure within these selected targets, not the full
historical donor suite. That unchanged suite reports 27 failures: the earlier
19 plus eight shared-allocator lifetime conflicts; the counts are not a full
regression pass. The three malformed-permission retirements are approved, but
blocktype-zero remains unresolved.

Actual consumer qualification now also requires cached contexts to hand off
sequentially on the same caller allocator, without a spurious retained-borrow
`busy` failure, and preservation of the three-argument absolute-deadline
`timed_run`/`interrupt_options` surface. The coordinator identified these as
Spine consumer gates; their respective owners are investigating/porting them.
Neither API/lifetime break is approved by the capacity, malformed-permission
or raw-entry decisions. Fresh AOT allocator tests, real consumer compilation
and product workflows remain separate gates.

Later coordinator checkpoints on 2026-10-16 report **31 public cases / 374
assertions** and **7 watchdog cases / 73 assertions** passing. These do not
silently relabel the earlier 27-case freeze scope or historical donor failure
record: the Spine-shaped watchdog overload and cached-context handoff remain
pending consumer gates. Fresh root AOT at the current pin/profile, codegen 5
and host ABI 3 reports **32/33 PASS**. The remaining heavy 32,768-byte logical
frame case returns typed `execution_unavailable` with a reported 32,872-byte
native frame and 64 MiB stack. The AOT owner confirms a structured known
native-stack error, not unknown/operand/OOM; diagnosis remains with that owner.
This is not complete 32 KiB/depth-1024 physical-envelope qualification.

The passing session test explicitly expects fail-closed behavior when exact
prepared resident reservation is unavailable. It is not successful session/cache
admission, execution or lifecycle acceptance. Exact pre-load reservation/hooks
remain future work, not an accepted post-load estimate. No product-ready pin
or complete migration is established by these scoped counts.

## Executable Boundary Matrix

The [standalone qualification project](../../tests/vm_wamr/CMakeLists.txt) tests
private engine mechanics. Its results do not certify public `backend`, the SDK
executor, installed consumers or full product workflows.

| Case | Expected result | Evidence/gate |
| --- | --- | --- |
| 65,536 i32 locals, three i64 parameters, one i32 operand | Reject in interpreter admission and compiler source admission; no AOT artifact | `capacity_probe.cpp`, profile rejection test |
| 8192/16384 bytes of locals plus one i32 operand, parameters excluded by flags | Exact budget accepts; lowering caller budget by one byte rejects with measured `function_bytes` | Profile boundary probe |
| Same accepted controls compiled to AOT | Check original source budget, then load and execute accepted native output; source bytes unchanged | Profile AOT test; not independent admission of an untrusted native artifact |
| 8/16 KiB with configured 251/1024 depth | Exact depth accepts, next call rejects; trap/cancel then reuse succeeds with sufficient provisioned stack | Preserved diagnostic evidence; public rerun required |
| Combined parameters/locals/operands, including caller flags zero | Exact shared byte cap accepts, one additional i32 rejects with `function_storage_bytes`; caller flags cannot bypass profile; compiler refuses original excess source before codegen | Shared metric and compiler-helper source admission pass; all signature/offset shapes and public integration pending |
| Source memory minimum at 65,535 and 65,536 pages | With otherwise valid matching fixtures, the proposed exact cap passes source-only profile admission; one-page excess gets typed refusal before instantiation; compiler uses the same predicate, publishes no artifact for excess and leaves source bytes unchanged | Fresh compiler helper boundary/public validator cases PASS; expanded private runtime/compiler fixture comparison NOT_RUN; no 4 GiB allocation implied |
| Explicit page configuration at the cap and above it | 65,535 is representable; 65,536 through `UINT32_MAX - 1` are typed profile/config refusal before narrowing/effects; `UINT32_MAX` retains its profile-intersected sentinel meaning | Selected local public cases PASS; all-mode/installed qualification remains open |
| Small initial memory with declared max 65,536 or no max | Source admission accepts the initial memory; actual growth remains capped by common/caller/module policy | Fresh private source fixtures added; public growth evidence separate, NOT_RUN here |
| Valid zero-initial memory and first growth | Zero remains a genuine zero-sized memory, not overflow; first grow obeys caller/module/profile caps, preserves base/data and zeroes new pages | Selected local public/private-system cases PASS; fresh AOT allocator/all-mode qualification separate; distinct from 65,536-page refusal |
| Heavy values live across recursive AOT calls | Sufficient provisioning executes the admitted case; deliberately insufficient provisioning fails operationally without native stack corruption | Full 32 KiB by depth-1024 physical gate open; padded-local passes do not close it |

For the fresh memory matrix, distinguish declared minimum, declared maximum,
caller configuration and effective growth limit. Cover both interpreter modes,
validation-only preparation, compiler source admission and the source-plus-native
bridge using the same frozen profile. Existing declared-maximum/absent-maximum
and unbounded-option semantics need explicit tests, not an assumption that a
small initial memory proves support for a 4 GiB allocation. Check limits in wide
arithmetic before any conversion to the callback's byte-count type. Record typed
refusal and no guest effects separately from physical allocation-failure tests.
Historical usage-0 results below do not close any of these revised page gates.

The owned `capacity_probe.cpp` now generates five source-only memory fixtures:
zero initial, exact cap, one-page excess, a small initial memory with maximum
65,536, and a small initial memory without a maximum. It checks exact logical
page metrics and typed private refusal without creating an instance. The
`profile_rejection.cmake` harness compiles the four admitted sources, rejects
the excess without an artifact, compares the same source predicate in the
runtime probe and checks unchanged inputs. Existing two profile CTest
registrations cover this added code; no CMake ownership change is needed.
Status of these additions: IMPLEMENTED, NOT_RUN in the finite standalone
checkpoint below because full builds failed before CTest. This is not public allocator/configuration
or large-memory execution evidence.

Do not use CTest `WILL_FAIL` to bless any nonzero subprocess exit. Expected
rejection needs a healthy accepted control, an identified rejection path, no
artifact publication and a successful process exit from the assertion harness.
Crashes, signals, timeouts and unrelated exceptions fail the test. A private
loader diagnostic assertion is not typed public profile rejection evidence.

## Profile Probe Results

On 2026-10-16, fresh Darwin ARM64 builds against fork pin `7459d1e` and build
profile digest `f8a7c360400a71084c9224ce9302965f56e51959136d0f56c99b638535dae7fa`
passed the strengthened two named profile CTests in each of four lanes:
**8/8 PASS**. This rerun includes 32 KiB AOT execution and compiler source
rejection of the +4-byte excess, not only the earlier 8/16 KiB controls.
The Forge private helper sources were pending changes, not a published release.

| Lane | Local build directory | Result |
| --- | --- | --- |
| Classic Release | `build/wamr-profile-classic-20261016` | 2/2 PASS |
| Fast Release | `build/wamr-profile-fast-20261016` | 2/2 PASS |
| Classic ASan/UBSan | `build/wamr-profile-asan-20261016` | 2/2 PASS |
| Fast ASan/UBSan | `build/wamr-profile-fast-asan-20261016` | 2/2 PASS |

Each directory retains `profile-final-results.xml`, CTest logs and generated fixtures
under `profile-rejection/`. The pinned private compiler was rebuilt in the
separate `build/wamr-profile-compiler-20261016` directory; its existing rejection
test also passed (1/1; `profile-final-compiler-results.xml`). Runtime sanitizer lanes instrument the runtime and Forge
probe, not the compiler or generated AOT machine code. No sanitizer findings
were emitted by these profile tests.

The [capacity probe](../../tests/vm_wamr/capacity_probe.cpp) asserts the exact
private loader rejection, verifies 8192/16384-byte healthy controls and typed
caller-budget refusal, checks the declared combined profile cap and one-i32
excess, and admits a healthy module after rejection. The
[AOT admission harness](../../tests/vm_wamr/profile_rejection.cmake) requires a
matching rebuilt compiler identity, successful native controls, unchanged WASM
and clean rejection of both the extreme fixture and the 32 KiB +4-byte source
without publishing an artifact. The compiler helper now enforces the same
production source-owned `engine_limits::check_profile` before codegen; no
test-only admission callback is installed. It does not invert arbitrary failures.

The exact combined fixture contains 24 parameter bytes, 32,740 local bytes and
one four-byte operand: 32,768 bytes total. Its next representable i32-local
excess is 32,772 bytes. The shared metric asserts typed `function_storage_bytes`
values; the compiler subprocess is checked for its specific clean diagnostic
and lack of output, not used as proof of typed public exception transport.

The 65,536-local WASM remains byte-identical to the historical fixture:
SHA-256 `d722434d3c26e3b24733f40143c5eb35cf0dc9df07211517330c275b3e9c1a30`.
Historical `FORGE_WAMR_CAPACITY_BASELINE` mode remains an explicitly named old
implementation oracle, not a fallback in the WAMR probe.

After configuring a selected standalone build, reproduce with:

```sh
cmake --build build/wamr-profile-classic-20261016 --target wamr_capacity_probe -j 4
ctest --test-dir build/wamr-profile-classic-20261016 -R '^wamr_profile_' \
  --output-on-failure --output-junit profile-final-results.xml
```

These historical usage-0 tests do not qualify the 16 MiB module ceiling, every
parameter/result or fast-offset shape, whole initialization, typed public profile/resource errors,
physical deep-stack provisioning, compiler process limits, public runtime or
installed consumers. Those gates remain open despite the 8/8 result.

### Rerun At The Reviewed Initialization Pin

The same strengthened profile tests were rebuilt and rerun on
`65f974bebba846e14e026552aaaa0b898b6f9498`, with the same build-profile digest:
**8/8 PASS**. The compiler was rebuilt from this pin, not relabeled from
`7459d1e`. Every WASM/AOT control below was generated in a new build directory.
Earlier directories and XML remain historical records.

| Lane | New build directory | Result |
| --- | --- | --- |
| Classic Release | `build/wamr-profile-classic-65f974b-20261016` | 2/2 PASS |
| Fast Release | `build/wamr-profile-fast-65f974b-20261016` | 2/2 PASS |
| Classic ASan/UBSan | `build/wamr-profile-asan-65f974b-20261016` | 2/2 PASS |
| Fast ASan/UBSan | `build/wamr-profile-fast-asan-65f974b-20261016` | 2/2 PASS |

Each new directory retains `profile-results.xml` and the generated source/AOT
fixtures under `profile-rejection/`. The fresh compiler build is
`build/wamr-profile-compiler-65f974b-20261016`; its separate rejection test passed
1/1 and retains `profile-compiler-results.xml`. No runtime sanitizer findings
were emitted. Compiler/generated-native-code instrumentation is still outside
these sanitizer lanes.

```sh
cmake --build build/wamr-profile-classic-65f974b-20261016 --target wamr_capacity_probe -j 4
ctest --test-dir build/wamr-profile-classic-65f974b-20261016 -R '^wamr_profile_' \
  --output-on-failure --output-junit profile-results.xml
```

This rerun selects only the two `wamr_profile_*` tests. It neither reruns nor
claims the coordinator's 46 imported-initialization registrations, full public
bridge acceptance or the full standalone suite. The 32 KiB source+AOT pair was
handed to the public bridge owner for independently scoped `prepared_module`
qualification. All profile-wide promotion gates remain partial.

After this passing run, the shared engine gained the out-of-line
`engine_exception::inspect` helper. The subsequent classic probe rebuild failed
to link that symbol because its source was not yet registered in the standalone
probe target. The coordinator owned that CMake update. The earlier 8/8 remains
valid historical evidence for its tested source state; the latest helper state
still requires all four lanes to be rebuilt/rerun.
This build-integration failure is not a capacity regression result.

The coordinator subsequently registered `engine_exception.cpp` in the standalone
target, resolving that missing-source wiring. The four profile lanes have not
been rerun after this change. The next reviewed pin is now `66e6be6`; full
usage-1 runs remain deferred until the private allocator/provider and centralized
compiler-profile freeze, not a new expensive rerun on `65f974b` or reuse of
usage-0/preceding-profile compiler inputs.

Current root optional compiler/helper/export wiring configures successfully.
The former OpenSSL header/build blocker is historical: the coordinator's current
OpenSSL 4 root build includes the real execution target. None of this is
installed/runtime-only package acceptance, which remains NOT_RUN.

### Focused Public Interpreter Evidence

The coordinator's independent historical root build at `65f974b` passed
`test_forge_vm_wasm_execution` in 0.25 seconds. The
[public runtime tests](../../tests/vm_wasm_execution/public_runtime_tests.cpp)
exercise the ordinary `prepared_module`/`runtime_context` path. The owning
runtime CMake selects the classic interpreter, so this is a local classic pass,
not an all-mode or cross-platform result.

Exact checked scope:
- Combined 32 KiB parameter/local/operand admission and +4-byte typed profile
  rejection with caller accounting flags zero.
- Successful initialization/call of the admitted module and structural module
  memory accounting (not peak resident allocation measurement).
- Recursive frames at 8192, 16384 and 32768 bytes, each at configured depth 251
  and 1024; exact depth succeeds, one extra call is refused and shallow reuse
  succeeds. These fixtures pad unused locals and do not prove live native frames.

Workspace evidence record `wamr-public-migration-20261016` retains
`integration-build.log` and `integration-ctest.log`. The same CTest invocation
also ran `test_forge_vm_wasm_invocation`, which failed one of its 14 cases at
guest memory-trap classification on the old structured-getter pin. The combined
run was 1/2 PASS, not a complete VM suite pass. The subsequent
[public checkpoint](../../tests/vm_wasm/WAMR_COMPATIBILITY.md) also records one
SDK execution trap failure on that old pin after unknown traps were made
operational, rather than falsely canonical. Thus the earlier execution PASS is
not a current target-wide green status. The getter fix is now pinned; current
usage-1 typed trap/resource transport still requires fresh runs. No string-based
runtime classification workaround is accepted.

## Remaining Physical Qualification Plan

This is a focused plan for the existing interpreter and AOT owners, not another
runtime, scheduler, parser, stack manager or set of production defaults.

1. Extend the existing public recursive test matrix after the provider freeze. Keep
   the successful classic cases as baseline and qualify fast and AOT through
   the same public source/native/context path. Record actual source metrics,
   interpreter frame reservation and the native stack mapping used. Count
   initialization/imported callbacks in the 1024 logical-depth budget; a test
   marker must not accidentally introduce a 1025th call.
2. Verify the current interpreter sizing mechanism against actual mode-specific
   frames: `S = (max_frame_bytes + 256) * (depth + 2) + 65536`. Here
   `max_frame_bytes` comes from upstream frame layout, actual physical stack
   cells, classic alignment/control blocks or fast constants/preserved slots,
   not just the 32 KiB logical limit. Check arithmetic/representability and
   allocation failure separately. This is the current implementation formula,
   not a proved universal capacity bound. The arithmetic product 32 KiB times
   1024 is 32 MiB; it is not a measured physical minimum or an accepted default.
3. Add the existing AOT owner's non-tail-recursive live-value fixture. Produce
   opaque runtime values before recursion and consume them observably after
   the recursive call so LLVM cannot remove padding, hoist an aggregate checksum
   or turn the fixture into tail recursion. Verify the emitted native frames
   and `.aot_stack_sizes` metadata via existing Forge descriptors, not merely
   the WASM local count. Use the executor's real mapped stack/guard and host
   bounce, not a fake boundary pointer or a duplicate stack-switch primitive.
4. Exercise sufficient and deliberately insufficient local provisioning. Budget
   the measured live call chain, VM/host entry transitions, existing reserve,
   guard/alignment and nested-host headroom independently from the interpreter
   value stack. A sufficient local configuration must execute the admitted
   case; insufficiency must be typed local unavailability without a crash or a
   canonical guest result. Frame metadata alone is not peak-stack measurement.
5. Repeat focused trap/cancel/reuse and A/B cancellation isolation with the heavy
   fixture. Record peak allocations and mapped/resident stack use for a bounded
   concurrent test configuration. Restore FP/depth state and release resources
   after every failure. Do not claim forced interruption of arbitrary blocking
   host callbacks, product effect rollback or full profile readiness from this
   matrix.

## Finite Standalone Checkpoint

On 2026-10-16, after the explicit public freeze, the four owned warm directories
were reconfigured/rebuilt sequentially at fork `66e6be6`, common profile
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`,
using LLVM 22.1.8 and `-j 4`. No duplicate root build or further variant was
launched. The final attempt is retained under
`build/wamr-profile-reports/66e6be6-shared-profile-20261016-frozen-v3`.

| Build/test scope | Result | Evidence in that report directory |
| --- | --- | --- |
| Fresh compiler helper/profile/layout CTests | 4/4 PASS | `compiler-results.xml`, `compiler-ctest.log`; matching binary sidecars |
| Classic Release full standalone | BUILD_FAILED; runtime CTest NOT_RUN | `classic-build.log` |
| Fast Release full standalone | BUILD_FAILED; runtime CTest NOT_RUN | `fast-build.log` |
| Classic ASan/UBSan full standalone | BUILD_FAILED; runtime CTest NOT_RUN | `asan-build.log` |
| Fast ASan/UBSan full standalone | BUILD_FAILED; runtime CTest NOT_RUN | `fast-asan-build.log` |

All four builds stop generating `fixture.aot`/`start.aot` through the upstream
`wamrc` CLI. Logs record SIGBUS/Bus error and SIGTRAP/Trace-BPT failures; one
command also exits 137, whose cause is not classified here. Library-helper AOT
generation succeeds, but does not replace the failed CLI lane. No runtime suite
count, sanitizer success or new private page-probe pass is claimed. Crashes are
qualification failures, not expected profile refusal.

Source inspection found `wamr-compiler/main.c` assigns raw `malloc/realloc/free`
to `Alloc_With_Allocator`, while the current common ABI enables usage-aware
callbacks. This is an actionable suspected CLI ABI mismatch, not a debugger-
confirmed crash diagnosis. Fork/compiler owners were notified; neither vendor
source nor coordinator-owned CMake was edited by this sidecar.

The initial attempts remain as `frozen-v1` and `frozen-v2`: they record a runner
sidecar/symlink copy failure, Bash-3.2 empty-array handling and missing standalone
Boost header discovery, not runtime admission results. The runner recipe below
is corrected. To reach the actual CLI failure, final configure explicitly used
the installed host Boost include directory via
`CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES`; `host-dependency.txt` records it.
No WAMR/profile flag was overridden. Portable Boost discovery/target wiring in
standalone CMake remains coordinator-owned, not qualified by that local include
path. Previous small cache/CTest metadata and earlier profile XML remain
preserved. `source-inputs-before.sha256` and `source-inputs-after.sha256` match
for all recorded actual compiler/runtime/probe inputs in the final attempt.

This finite checkpoint has ended. Resume only after the owning coordinator
addresses the CLI/dependency blockers and confirms any affected source freeze;
do not repeat builds or widen capacity to turn this record green.

## Fresh Full Standalone Rerun Plan

Status: recipe retained for a later explicitly resumed run; the attempted
checkpoint above is BUILD_FAILED and runtime CTests are NOT_RUN. The
coordinator's standalone source wiring now calls
`forge_wamr_apply_profile(... runtime)` after the upstream include and includes
`engine_memory.cpp`; it no longer maintains a duplicate feature list. Do not
execute until the allocator owner freezes the private provider/dispatcher and
the compiler owner freezes the centralized shared profile/code-generation
settings. Full private probes use their real system provider; public allocator
tests must independently use the actual caller allocator. Neither a fake
provider nor a usage-0 override is permitted.

The centralized helper changes source identity again: even an earlier usage-1
binary is not automatically matching. Record the agreed final profile hash at
freeze and rebuild the compiler before configuring runtimes. No current hash
is declared final while that work is in progress.

Disk-constrained execution reuses only this sidecar's five completed standalone
directories below, incrementally and sequentially. Their `65f974b` directory
names are historical labels, not the next run's identity. Keep old profile XML
and random `profile-rejection/` evidence intact. Do not use another owner's
active build, run `--fresh`/clean, delete historical prepared sources, or create
a duplicate root build. Preserve the small old cache/CTest metadata separately
before mutable binaries and generated fixture targets are rebuilt.

Run from the repository root in Bash with the same approved LLVM 22.1.8
toolchain and matching guest `wasm-ld`. Set `LLVM_PREFIX`, `WASM_LD`, unique
`RUN_ID`, and the compiler owner's `FORGE_FROZEN_PROFILE_SHA256`; on macOS set
the selected `MACOS_SYSROOT`. Freeze booleans below record actual owner
confirmation, not substitutes for approval or implementation evidence. Only
the report directory is new; no source-tree CMake change is part of this recipe.

```bash
set -euo pipefail
: "${LLVM_PREFIX:?select the approved LLVM 22.1.8 prefix}"
: "${WASM_LD:?select the matching guest wasm-ld}"
: "${RUN_ID:?choose a new qualification run id}"
: "${FORGE_FROZEN_PROFILE_SHA256:?obtain the agreed final shared-profile hash}"
test "${FORGE_PRIVATE_ALLOCATOR_FROZEN:-no}" = yes
test "${FORGE_COMPILER_PROFILE_FROZEN:-no}" = yes
test "$(git -C vendor/wamr rev-parse HEAD)" = \
  66e6be6c626856758ada54ddac2fc81ae49f5648
test -z "$(git -C vendor/wamr status --porcelain)"
repo="$PWD"
compiler="$repo/build/wamr-profile-compiler-65f974b-20261016"
runtime_prefix="$repo/build/wamr-profile"
run="$repo/build/wamr-profile-reports/66e6be6-shared-profile-$RUN_ID"
test ! -e "$run"
mkdir -p "$run"
check_profile() {
  test "$(shasum -a 256 cmake/ForgeWamrProfile.cmake | awk '{print $1}')" = \
    "$FORGE_FROZEN_PROFILE_SHA256"
}
save_metadata() {
  local build="$1" label="$2"
  test -f "$build/CMakeCache.txt"
  mkdir -p "$run/previous-$label"
  cp "$build/CMakeCache.txt" "$run/previous-$label/"
  for file in LastTest.log LastTestsFailed.log; do
    if test -f "$build/Testing/Temporary/$file"; then
      cp "$build/Testing/Temporary/$file" "$run/previous-$label/"
    fi
  done
}
check_profile
host_args=(
  -G Ninja
  "-DCMAKE_C_COMPILER=$LLVM_PREFIX/bin/clang"
  "-DCMAKE_CXX_COMPILER=$LLVM_PREFIX/bin/clang++"
)
if test "$(uname -s)" = Darwin; then
  : "${MACOS_SYSROOT:?select the macOS SDK}"
  host_args+=("-DCMAKE_OSX_SYSROOT=$MACOS_SYSROOT")
fi
shasum -a 256 cmake/ForgeWamrProfile.cmake \
  libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret/capacity_profile.cppm \
  | tee "$run/profile-hashes.log"
git diff --binary HEAD -- AGENTS.md cmake libraries/vm tests/vm_wamr \
  > "$run/forge-pending.patch"

save_metadata "$compiler" compiler
for binary in "$compiler/compiler/vendor/wamrc" "$compiler/wamr_compiler_probe"; do
  binary="$(realpath "$binary")"
  if test -f "$binary.forge-identity"; then
    cp "$binary.forge-identity" "$run/previous-compiler/"
  fi
done
cmake -S tests/vm_wamr/compiler -B "$compiler" "${host_args[@]}" \
  -DCMAKE_BUILD_TYPE=Release "-DLLVM_DIR=$LLVM_PREFIX/lib/cmake/llvm" \
  2>&1 | tee "$run/compiler-configure.log"
cmake --build "$compiler" -j 4 2>&1 | tee "$run/compiler-build.log"
ctest --test-dir "$compiler" --output-on-failure \
  --output-junit "$run/compiler-results.xml" 2>&1 | tee "$run/compiler-ctest.log"
identity="66e6be6c626856758ada54ddac2fc81ae49f5648-$FORGE_FROZEN_PROFILE_SHA256"
for binary in "$compiler/compiler/vendor/wamrc" "$compiler/wamr_compiler_probe"; do
  cmake "-DCOMPILER=$binary" "-DSOURCE_IDENTITY=$identity" \
    -P cmake/ForgeWamrCompilerIdentity.cmake
done

for lane in classic fast asan fast-asan; do
  check_profile
  fast=OFF
  case "$lane" in fast|fast-asan) fast=ON ;; esac
  runtime_args=("${host_args[@]}")
  case "$lane" in
    asan|fast-asan)
      runtime_args+=(
        "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
        "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
      )
      ;;
  esac
  build="$runtime_prefix-$lane-65f974b-20261016"
  save_metadata "$build" "$lane"
  cmake -S tests/vm_wamr -B "$build" "${runtime_args[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DFORGE_WAMR_FAST_INTERP=$fast" \
    "-DFORGE_GUEST_CLANG=$LLVM_PREFIX/bin/clang" \
    "-DFORGE_GUEST_WASM_LD=$WASM_LD" \
    "-DFORGE_WAMRC=$compiler/compiler/vendor/wamrc" \
    "-DFORGE_WAMR_COMPILER_PROBE=$compiler/wamr_compiler_probe" \
    2>&1 | tee "$run/$lane-configure.log"
  cmake --build "$build" -j 4 2>&1 | tee "$run/$lane-build.log"
  ctest --test-dir "$build" --output-on-failure \
    --output-junit "$run/$lane-results.xml" 2>&1 | tee "$run/$lane-ctest.log"
done
```

This builds all standalone targets and runs all registered tests without a
profile-only filter. Matching compiler sidecars are produced by their normal
build targets; do not hand-edit identity files. Preserve configure/build logs,
CTest XML, generated source/AOT inputs, actual test counts and sanitizer output.
The pending patch above omits untracked sources: retain a coordinator-owned
complete source snapshot/inventory too, without modifying or staging the shared
checkout. Confirm the centralized profile's effective definitions in compiler
and both runtime modes, including usage-1 ABI and the selected classic/fast
role; record the actual code-generation settings and provider used by each probe.
A failed build/provider hookup is
a qualification blocker, not a guest/profile refusal or a reason to skip tests.

Runtime sanitizer lanes instrument runtime/probe sources, not LLVM or generated
AOT instructions. Their pass would still not close public allocator ownership,
the full public suite, 32 KiB live-AOT-frame/depth-1024 physical provisioning,
installed consumers, cross-platform determinism or product rollback. Run and
record those gates separately; do not infer them from private full-suite counts.

## Evidence And Promotion Gates

External workspace records `wamr-limits-audit-20261016` and
`wamr-runtime-qualification-20261016` retain their original findings. They
precede the requirement change and are not rewritten. The latter qualified
selected real consumer actions, not full product integration or partial-effect
rollback. In-repository provenance starts with the
[instruction/depth report](forge-wamr-limits-depth-qualification-v1.md).

The [maintained fork](forge-wamr-fork-v1.md) pin is
`66e6be6c626856758ada54ddac2fc81ae49f5648`; that note records the actual source
URL and published fork PR 1. The baseline-to-pin range contains 11 commits and
29 changed files (946 insertions, 339 deletions). Its seven original patch commits
were transferred at `7459d1e`, whose 22 changed engine files match the qualified
prepared source byte-for-byte. Subsequent reviewed commits fix imported
initialization, structured errors, raw allocation-failure IDs and runtime/AOT
table alignment with checked native metadata offsets, without widening capacity.
The final usage-0 low-level follow-up passed **256/256** checks (184 initialization,
64 structured-error and eight allocation-failure checks across four modes) plus
**one Release layout probe**, with independent exact-diff review. These are the
bounded fork results in the provenance note, not the full standalone suite or
whole public/product acceptance. Current usage-1 qualification is still pending.

Build identity is the immutable fork revision plus the
[Forge build-profile](../../cmake/ForgeWamrProfile.cmake) digest. Historical
patch files are provenance only, not build inputs. Reconfigure or use a fresh
build for new identity checks; old prepared sources remain historical evidence.

The imported-initialization fix is not part of the historical `7459d1e` results;
its `65f974b` usage-0 profile rerun is recorded separately above. The current
`66e6be6` pin and usage-1 build-profile change require rebuilt compiler/runtime
qualification, not relabeling an old run. Optimizing artifact identity must
additionally cover the source-owned capacity contract (currently hashed from `capacity_profile.cppm`);
the standalone fork/build-profile sidecar alone is not an artifact acceptance
proof.

Fork/pin evidence alone does not close runtime-only source/install/relocation,
optional compiler packaging, absence of public vendor types/EOS linkage,
cross-platform numeric determinism, compiler-session/cache lifecycle, artifact
tamper rejection or product rollback gates. No downstream pin changes, merge,
deployment or production readiness are claimed here.
