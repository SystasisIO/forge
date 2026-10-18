# WAMR Replacement Experiment

Status: experimental, pending changes, NOT ACCEPTED. Updated: 2026-10-18.

This branch investigates replacement of both EOS-derived execution and the OC
implementation with WAMR interpreter, AOT (ahead-of-time native compilation)
and JIT (in-process compilation during execution). It does not enable a new
production backend. Passing the
standalone probe below is not feature completeness, deterministic consensus,
or downstream acceptance.

## Approved Completion Block

The maintainer approved this implementation block on 2026-10-16. It is work
scope, not a claim that any currently open acceptance gate passed.

1. Replace exact prospective preparation sizing with a conservative ceiling
   enforced before every owned allocation/mapping. Include temporary overlap and
   C++ bridge storage, reserve through existing inflight/resident accounts and
   refund unused reservation only after storage release. Document the scoped
   `prepared_byte_size()` contract change; no guessed multiplier is a proof.
2. Complete the existing compiler-session/cache cold/warm lifecycle, eviction,
   cancellation, parallel requests, corrupt-cache rejection and worker recovery.
3. Replace the remaining EOS-derived x86 JIT with an existing WAMR JIT mechanism.
   An AOT substitution, silent feature removal or fallback to EOS is not closure.
   Qualify ARM64 JIT only if the pinned WAMR implements it; do not develop an
   architecture backend solely for this migration.
4. Qualify installed/relocated Forge components and isolated real Spine/Turbine
   builds and execution. Interpreter/AOT runtime remains independent of LLVM;
   an explicitly optional LLVM JIT build may require LLVM in its process.
5. Independently review implementation, allocation/cancellation boundaries and
   residual executable EOS paths. Record platform-specific evidence; Darwin
   ARM64 results do not establish Linux or x86-64 acceptance.

This decision supersedes the exact-planner prerequisite and the blanket ban on
LLVM for an explicitly selected LLVM JIT configuration. It preserves LLVM-free
default runtime/installed consumers and all other approved requirements.

## Non-Negotiable Requirements

1. Preserve the consumer-facing Forge WASM guarantees while reviewing library
   ownership before API wiring. Consumers must not undergo broad architectural
   refactoring or acquire WAMR C types. A boundary correction needs an explicit,
   minimal source/package migration; it does not permit behavioral regression.
2. Deterministic observable execution across admitted inputs, interpreter/AOT,
   supported architectures, worker histories, and concurrent workloads.
3. Cover the required functional guarantees of both EOS VM and OC, including the
   in-progress ARM64 work, within the maintainer-approved bounded profile below,
   not just the version committed to `dev`.

No unapproved weaker feature profile, omitted failing test, silent EOS fallback, or
disabled safety check may be reported as completion. Expected differences from
historical EOS parsing quirks or NaN payloads need an explicit, reviewed
execution-profile decision. Replaying an old chain is not required, but
current donor contract compatibility remains required.

## Approved Capacity Decision

On 2026-10-16 the maintainer explicitly retired full historical extreme-capacity
compatibility as a replacement requirement. WAMR must use one versioned,
bounded deterministic admission profile for interpreter and AOT. This is an
intentional requirement change, not a claim that the old capacity failure was
fixed. Do not widen WAMR counters to admit the 65,536-local fixture; retain it
as an expected rejection in both admission paths. Consumer APIs, required donor
contracts, determinism, safety, cancellation and isolation remain mandatory.

The [public profile decision and boundary matrix](../donors/forge-wamr-public-profile-v1.md)
distinguish approved policy, measured diagnostic cases and unfinished public
integration. Exact capacity constants must be derived jointly with the public
interpreter and AOT owners. Passing tiny contracts does not define those caps.
The new [release note](../releases/unreleased-wamr-bounded-profile.md) names
the narrowed surface and migration; no backend switch or release is implied.
The [fork provenance note](../donors/forge-wamr-fork-v1.md) records the immutable
source pin and retirement of build-time patching; historical reports remain
unchanged.

Current shared P1 policy sets `max_function_results = 1`; valid multi-result
types are rejected by common admission before public signature projection.
The 16 MiB module, 32 KiB combined function storage, depth/control 1024 and
revised 65,535-page caps remain PROPOSAL until complete boundary/physical tests.
The page proposal derives from `UINT32_MAX / 65536`: 4 GiB minus 64 KiB.
The former 65,536-page proposal is withdrawn, not supported behavior; its byte
size wraps to zero at the u32 usage callback. The public owner is fixing typed
early source/configuration refusal, without widening counters or callbacks.
Valid zero-initial memory remains a separate case. Fresh exact/excess source
admission, compiler/runtime equality, configuration and zero/growth tests are
required; no prior run qualifies this revised cap.
The source also declares `max_instance_metadata_bytes = INT32_MAX`, derived
from prospective 64-bit AOT header/memory/global/table/extra-state extents and
its signed-offset representation. This is not a page cap; its fresh source-only
boundary/compiler/runtime qualification remains pending with the full profile.
The [neutral public trap contract](../../tests/vm_wasm/WAMR_COMPATIBILITY.md#neutral-trap-contract)
preserves `guest_trap : interpreter`, typed memory and operational-resource
distinctions; unknown/auxiliary-stack reasons must not become canonical guest
results by inference or string matching.

## Approved Compatibility Breaks

The human maintainer explicitly approved rejecting three historical malformed
WASM extensions: instructions after function end (`allow_code_after_function_end`),
invalid value types in empty locals groups (`allow_invalid_empty_local_set`),
and reserved memory/table flags (`allow_u32_limits_flags`). These permissions
are unsupported/deprecated in the new profile; current profile tests must
expect rejection even when the old flags are enabled. Preserve original donor
records and historical failures, and record fresh results separately. This
decision does NOT cover `allow_zero_blocktype`; its admission-policy issue
is repaired through the existing loader's numeric/legacy-zero modes. Fresh
compiler/runtime and public API evidence is tracked in the follow-up below.

A separate explicit approval removes only the old raw OC machine-entry API,
`artifact::entry()` and `entry_function`, with dependent surfaces confirmed by
the AOT owner. Source removal is confirmed, not complete runtime/release qualification.
Use the public executor with WAMR's own ABI, not a raw pointer adapter or an
always-throw compatibility placeholder. Keep `compiled_module`, host APIs,
executor and session/cache. The [release note](../releases/unreleased-wamr-bounded-profile.md#raw-oc-entry-api-break)
names the confirmed source surfaces and migration; no broader API removal is
approved. The coordinator's consumer trace found no product use of this Forge
raw-entry API; internal donor native calls do not establish such use.

## Dev Integration And Additional API Decision

On 2026-10-17 the maintainer confirmed that the current Turbine dependencies
already delivered to Forge `dev` must be included in this experiment. The new
working branch `vm-wasm-wamr-dev-integration-v1` starts at
`3a6e388f29c1535d9d9c642877d416ca7e52e83a` (Forge PR 187). The complete pending
WAMR source was transferred from `vm-wasm-wamr-experiment-v1`; the old worktree,
its staged changes and its build evidence are preserved. A source/hash manifest
and the original binary Git patch are recorded in workspace artifacts under
`wamr-dev-integration-20261017-r4`. Generated build outputs were not copied.
Three textual conflicts in API-WASM guest parity and test registration were
resolved by retaining the dev API additions and the WASM family rename. This
is source integration, not a completed product build or an accepted release.

The maintainer approved removing the old `interpreter::parser`,
`null_backend::parser/context`, `jit_allocator` and `growable_allocator`
surfaces. Preserve module validation, import/apply checks, live context reuse,
caller linear-memory ownership and bounded native allocation with WAMR.
The exact source changes and migration are tracked in the
[release note](../releases/unreleased-wamr-bounded-profile.md#legacy-parser-and-allocator-api-break).
Other host/value helpers are not included in this approval merely because
they originated in the donor.

Instruction-level `jit_profile` is explicitly retained as unfinished work.
The inspected Spring source `e6a99f68b67abc4d89fe716755b2e1394a4991f7`
selects its profiling runtime for `--wasm-runtime eos-vm-jit` with
`--profile-account`, samples at 200 microseconds, maps native PCs to WASM
offsets, and writes `.profile` plus the corresponding `.wasm`. Its implementation
is x86-64-specific. WAMR/LLVM function counters alone do not replace that
instruction mapping. The current candidate implements the native instruction
map and public observer; fresh runtime and delivery qualification remain open,
not an approved retirement or completed compatibility.

The profiling acceptance gate requires stripped WASM with several distinguishable
instructions inside one function, using original WASM file-byte offsets. Reuse
the WAMR translator and LLVM debug-line facilities; do not build a second WASM
parser or native disassembler. Resolve addresses after native linking, including
noncontiguous executable ranges. Unknown/prologue/host PCs and gaps must not be
attributed to an arbitrary guest instruction. Publish immutable, budgeted maps
before execution; translation during sampling must not allocate or lock, and
code/map lifetime must remain valid until sampling stops. Verify actual native
PC samples, cancellation, unload and reuse, not only a generated metadata table.
Optimized-away instructions need not have their own native range; function-only
mapping is insufficient. This gate does not change ordinary-JIT dependencies or
enable profiling by default.

Turbine's JSON/HTTP/SSE/secrets API gap is addressed by this dev base. Actual
Turbine acceptance still requires a matching installed SDK and build/test run.
Spine's separate ObjectDB allocation/snapshot feature dependency is not part of
PR 187; updating to this dev base alone does not close its full-process gate.

## Current Qualification Checkpoint

The [2026-10-18 follow-up](../donors/forge-wamr-dev-integration-20261018.md)
records published draft candidate `12d1f906668cde93c03e7d864e71ee1e10dfb194`, public
profiling, length-aware export names and recorded current-dev prerequisites.
Independent source reviews are clear within their stated scope. Fresh combined
tests, installation and isolated product acceptance are in progress; older
passes below do not automatically qualify this candidate.

The [2026-10-17 dev integration checkpoint](../donors/forge-wamr-dev-integration-20261017.md)
records the fresh combined JIT/compiler build, 61/64 CTest matrix and remaining
unit/spec failures. Its 45/45 AOT and 7/7 native-JIT subsets do not close the
full compatibility or product gates. Follow-up closed the zero-depth typed-error
regression (2/2 focused cases) and ran the actual generator mapping (1/1).
The private instruction-profile prototype passed a native test and three repeat
runs; independent review found no P1/P2 source defect in its frozen scope.
Public profiling, early-failure/lifetime qualification and product acceptance
remain open. Historical spec differences were checked against current WASM
rules before proposing engine patches; only the NUL-containing export-name
limitation remains a demonstrated compatibility gap in that spec subset.
The following results remain evidence
for the preserved pre-integration worktree only.

The preceding clean source pin was
`ed5c12ae6bcec88f2863aa7c541b6d829d657a4c`. The
[fork provenance](../donors/forge-wamr-fork-v1.md) records the new source-reviewed
LLVM JIT implementation. On the preserved pre-integration worktree, Darwin
ARM64 native JIT probes and public runtime tests passed 12/12 CTest targets;
the clone/error-TLS test changes received independent scoped review. This
does not qualify the new dev integration, instruction-level profiling, Linux,
x86-64, installed JIT packaging, or full product processes.
The matching AOT helper/runtime on the preserved worktree passed 45/45 CTest
targets, including 8/16/32 KiB heavy frames at depth 1024, reservation overlap,
late cancellation and weak-pin eviction/refund. Pin `ed5c12a`, host ABI 3 and
codegen 5 match across helper/runtime/request. The rerun records are named
`aot-ed5-resident-r3-20261016T201302Z-*`; earlier failed runs remain preserved.
These source-specific results do not qualify subsequent source integration.
The [current follow-up](../donors/forge-wamr-follow-up-20261016.md) records the
earlier `75e8f91` and `156dbd0` policy, allocator, preparation, public/installed
checks and remaining gates. Those results do not automatically qualify a new
pin. The following subsection preserves the earlier same-day evidence.

### Earlier Same-Day Checkpoint

The preceding source pin was `66e6be6c626856758ada54ddac2fc81ae49f5648`.
The [fork provenance](../donors/forge-wamr-fork-v1.md) records its source URL,
published PR 1 and reviewed changes: 11 commits, 29 files, 946 insertions and
339 deletions from the upstream baseline. The final low-level usage-0 follow-up
passed 256/256 checks and one Release layout probe with independent review.
This closes that bounded fork follow-up, not the full standalone suite, public
interpreter/AOT behavior, allocator integration or product acceptance.

The common profile now enables `WAMR_BUILD_ALLOC_WITH_USAGE=1` for the approved
existing-allocator usage dispatcher. Source implementation is present: one stable
process dispatcher and scoped owners, exclusive instance borrow of the caller's
`wasm_allocator`, no second linear-memory buffer and no public system fallback.
Historical usage-0 binaries/results are not evidence for this new identity.
Standalone CMake now registers `engine_memory.cpp` and applies the shared runtime
profile after the upstream include, without its own duplicate feature list.
The compiler owner is centralizing the common profile/code-generation settings,
so even a preceding usage-1 identity is not current matching evidence. A rebuilt
matching compiler and all four full standalone lanes wait for the private
allocator/provider and shared compiler-profile freeze. Reuse owned completed
standalone build directories incrementally; no duplicate root builds. Commands and prerequisites are
in the [profile rerun plan](../donors/forge-wamr-public-profile-v1.md#fresh-full-standalone-rerun-plan).
The allocator owner now reports common production source frozen; this does not
establish the compiler's final matching freeze or close unrun gates. Full
standalone/public promotion remains pending; scoped new public results belong
to their owner's separate evidence, not the historical usage-0 run.
The coordinator independently verified the new public checkpoint XML:
execution 27/27 cases (244 assertions), private memory/metadata 4/4 (130),
selected CTest targets 4/4. See the
[current scoped record](../donors/forge-wamr-public-profile-v1.md#current-scoped-public-checkpoint).
Actual consumer gaps remain: sequential cached contexts sharing one allocator
need correct borrow handoff, and three-argument absolute-deadline
`timed_run`/`interrupt_options` compatibility is being ported by its owner.
These are not approved breaks and are not covered by private profile passes.
Later same-date coordinator evidence reports public 31 cases/374 assertions
and watchdog 7/73 passing; the actual consumer-shaped overload/handoff gates
remain pending. Fresh root AOT is 32/33: the heavy 32 KiB case still reports
typed physical `execution_unavailable` (native frame 32,872 bytes, 64 MiB
stack). A passing fail-closed session-reservation test is not successful
session/cache acceptance. Do not promote the whole 32 KiB physical envelope.

The finite private standalone checkpoint ended: fresh compiler CTests **4/4
PASS**, all four warm runtime builds **BUILD_FAILED** at the upstream `wamrc`
CLI generating AOT fixtures; runtime CTests **NOT_RUN**. No extra variants/root
duplicates were launched. The [finite report](../donors/forge-wamr-public-profile-v1.md#finite-standalone-checkpoint)
records logs, matching source hashes, local Boost include discovery, runner
failures and the suspected usage-callback CLI ABI gap. This is not a profile pass.

## Baseline And Ownership

Base: fetched `origin/dev` at
`8975f234dabf2cbfb91d766bf32194f3d8832a5b`.
The comparison worktree is `vm-wasm-optimizing-b40-v1` at the same commit plus
pending changes. Its complete selected file inventory is captured in the
[baseline manifest](../donors/forge-wamr-baseline-v1.json). The local build
directory also retains a source archive. Neither source worktree nor product
pins are modified by this experiment.

- Preserve `forge::vm::wasm::execution::compiled_module`, invocation/result and
  cancellation usage wherever possible. Preserve the guest SDK's Clang/lld
  pipeline, WASM imports, ABI generation and canonical intrinsic registry.
- Keep `forge::vm::wasm::interpret` and `forge::vm::wasm::optimizing` as mode
  boundaries. WAMR is a private implementation, not a public namespace family.
- Keep AOT compilation in an optional isolated worker backed by `vm/wasm/compiler`.
  Ordinary interpreter/AOT runtime consumers must not link LLVM. Explicit LLVM
  JIT builds are the approved private dependency exception. Neither path may
  compile on an I/O executor or acquire a second job scheduler.
- Adapt existing compiler-session/cache ownership; do not reimplement upstream
  AOT loading, memory management or native code generation.
- Existing public options must be enforced, explicitly rejected when not
  supported yet, or individually migrated with approval. Ignoring an option
  violates compatibility. Do not blanket re-export WAMR types as Forge API.
- Compiler artifact payloads will change. Reject old/foreign payloads using
  engine/version/target/profile identity and rebuild from original WASM. Cache
  format invalidation does not authorize changes to persisted contract state.

## Library Boundary Review

Placement approved by the maintainer and reflected in AGENTS. The source
rename is implemented; WAMR public API replacement is still incomplete. See
the [migration note](../releases/unreleased-wasm-family-boundaries.md) for exact
source/package names and independently recorded validation.

| Leaf | Ownership and dependency direction | Implemented state |
| --- | --- | --- |
| `vm/wasm/interpret` | Neutral VM and shared WAMR bridge; no SDK/Chain/execution/tooling imports | Local usage-1 public handoff and four private runtime lanes pass; installed/downstream and cross-platform gates remain open |
| `vm/wasm/optimizing` | Optimized executor and existing bounded session/cache; consumes the neutral interpret bridge; no LLVM in runtime | Selected live AOT gates pass 36/36; exact preparation and successful session/cache remain open |
| `vm/wasm/compiler` | Optional WASM-to-native compilation, private LLVM dependency | Current-profile CLI/library checks pass 7/7 each in Release and sanitizer builds; installed API acceptance NOT_RUN |
| `vm/wasm/execution` | Bounded Contract SDK action adapter over the VM; may depend on SDK/Chain value types | Moved from `execution/wasm`, independent target/component/module prefix; selected public tests pass 35/35, not full product acceptance |

Neither `vm` nor `wasm` exposes an aggregate module or target. Directory-family
membership does not make the lower VM layers depend on the SDK action adapter.

- **VM mechanisms:** loading, validation, scalar values, checked linear-memory
  access, generic host calls, numeric behavior and interruption belong to
  `interpret`. The agreed shared bridge uses `prepared_module` for original
  source admission and source-plus-native loading, with `runtime_context` in
  both modes. These are neutral Forge contracts, not WAMR public types. The
  source-plus-native overload must check original WASM before native use; its
  agreed shape is not evidence that implementation/integration has passed.
  Do not duplicate WAMR initialization, instances, memory or host bridges in
  `optimizing`, and do not introduce a parallel `core` facade.
- **Compilation:** the reusable compiler belongs to optional `vm/wasm/compiler`.
  A tooling command retains only process-envelope/resource setup and compiler
  invocation. The comparison baseline's OC library has not been blindly copied
  here. Public compile request, artifact and session adaptation is present but
  still requires runtime/installed qualification; a passing private helper does
  not replace those contracts or their acceptance tests.
  LLVM isolation is enforced through targets, not directory separation.
- **SDK action execution:** `vm/wasm/execution` accepts SDK receiver
  and action names, requires `apply(i64,i64,i64)`, selects approved imports and
  bounds input/output/console. These are SDK invocation semantics, not generic
  VM behavior. Keep this separate leaf above neutral VM leaves in the dependency
  graph, within the same physical WASM family. Chain names/import policy do not
  move into `interpret`, `optimizing` or `compiler`.
- **Lifecycle:** retain the existing compiler-session, cache and worker owner.
  Boundary cleanup must not introduce a second scheduler, process manager or
  cache, and public guest execution APIs must not import a compiler command or
  LLVM. The explicitly selected JIT implementation may link LLVM privately.

The migration note records paths, targets, package components and module
prefixes. Shared value/profile/artifact contracts must not make the compiler
import the executor/session. The optimized executor consumes real neutral
execution mechanisms through the agreed interpret bridge, not a copied backend
or a dependency introduced merely to obtain common types.
Generic VM code must not import SDK action execution, Chain, testhost or tooling
commands. SDK-specific host registrations derive from the canonical intrinsic
registry in the adapter; the VM host-call mechanism stays generic.

Acceptance requires installed runtime-only builds without LLVM/compiler tooling,
an independently consumable optional compiler, import/link boundary checks and
real consumer builds after any approved mechanical migration. Update AGENTS,
owning READMEs, package exports and migration notes together when implementing
the approved layout. No numeric, cancellation or donor-compatibility gate is
waived by moving code. Root optional compiler targets, the compiler helper and
package export wiring now configure successfully. This is source/configuration
integration, not a verified installed package. Runtime-only installation,
relocation, absence of LLVM in runtime consumers and optional installed compiler
consumption remain NOT_RUN until their own acceptance tests execute.

## Acceptance Matrix

All rows are mandatory. `NOT_RUN` is not a pass. Probe coverage is evidence for
only the named subcase, not closure of a row.

| Gate | Required evidence | Current status |
| --- | --- | --- |
| Boundaries | Reviewed owner/dependency map; no VM-to-SDK/tooling reverse imports, duplicate engine owners or mandatory LLVM; explicit migration for any approved rename | PARTIAL: layout implemented and source build qualified; public compiler/optimized migration open |
| API | Installed package consumer compilation; documented minimal binding edits; no vendor types in public modules | PARTIAL: current local usage-1 selected CTest 4/4 and execution 27/27 pass; old-pin trap failures remain historical. Installed/full AOT, cached-context allocator handoff and absolute-deadline watchdog consumer compatibility remain open |
| Profile | Versioned constants, equal interpreter/AOT admission, retained rejection fixtures, explicit migration; no silent clamping | Bounded policy and P1 single-result rejection ACCEPTED; other numeric caps PROPOSAL. Historical usage-0 32 KiB/+4 and 65,536-local source-admission cases pass classic/fast/AOT; fresh usage-1/full public/physical profile open |
| Admission | Every current option classified and enforced within the approved profile; malformed modules, forbidden imports, signatures, limits, start and exports | PARTIAL: three named malformed-parser permissions explicitly retired; new-profile rejection tests required, historical evidence unchanged. `allow_zero_blocktype` is not waived; full options/early load bounds/public wiring remain open |
| Host bridge | Canonical registry-generated imports, bounds/alignment/NUL/overflow checks, nested calls, compiler builtins and host error propagation | PARTIAL: private scalar/range/string bindings and typed host failures qualified in interpreter/AOT; production registry/converters and full SDK open |
| Donor contracts | Original required donor WASM; ABI, DB, authorization, inline actions and financial FP scenarios | NOT_RUN |
| Numeric | Full f32/f64 operation/conversion corpus, NaN/sNaN, signed zero, subnormals, min/max, rounding, constant folding and FP-state restoration | PARTIAL: local ARM64 corpus passes; full/cross-platform gate open |
| Resources | Equal logical limits; deterministic growth policy; local allocation/stack failure never masquerades as a canonical contract result | PARTIAL: historical local public classic padded-local matrix passes; usage-1 allocator and typed page-boundary refusal in progress. Revised 65,535/65,536-page source/config/compiler/runtime and zero-initial-memory tests NOT_RUN; full physical profile/live AOT/concurrency/allocation/growth/lifetime gates open |
| Start and cancellation | One invocation scope covers instantiation/imported or defined start/post-instantiate/apply; cancellation before/during start, pure loops and host boundaries | PARTIAL: imported initialization fix included in 66e6be6; reviewed usage-0 follow-up includes 184 initialization checks. Fresh usage-1/full public deadline/admission scope and blocking host calls remain open |
| Isolation | Fresh memory/globals/tables, nested and parallel calls; cancel A while B is active; safe reuse or typed poison after every failure | PARTIAL: tested A/B and nested FP; complete failure policy open |
| Native execution | Interpreter cold start without compiler; optimized execution on Linux x86_64 and macOS ARM64; Linux ARM64 tracked separately | PARTIAL: macOS ARM64 probe only |
| Compiler process | Bounded jobs/bytes, deadlines, cancellation, crash/restart and shutdown; no stuck child or zombie | NOT_RUN |
| Artifacts/cache | Identity, compiler provenance, tamper rejection, atomic publication, quota/eviction/invalidation and rebuild | Full acceptance NOT_RUN; the current passing session test expects fail-closed exact-reservation refusal, not successful session/cache use |
| Product effects | Correct rollback; no replay/fallback after effects without rollback; retries preserve the original deadline | NOT_RUN |
| Packaging | Runtime install/relocation without LLVM; SDK/testhost/validator use new implementation; no EOS implementation linked transitively | NOT_RUN for installed/runtime-only acceptance. Prior OpenSSL header failure is historical: coordinator root OpenSSL 4 build included real execution; optional compiler/helper/exports configure. Neither is fresh usage-1 execution or installed qualification |
| Performance | Same corpus/configuration: cold/warm latency, native throughput, peak memory and compilation cost; accepted regression budget | NOT_RUN |
| Provenance | Residual EOS-derived parser/numeric/conversion/allocator/OC dependencies removed from replacement targets; required notices retained | NOT_RUN |

Wall-clock cancellation is a local interruption, not a deterministic contract
result or a replacement for resource billing. Introducing fuel (an instruction
budget) requires a versioned policy; it must not silently replace existing CPU
accounting. Native stack bytes are not equivalent to logical call depth.

The new profile must define observable floating-point bits and resource
behavior before production. Merely setting `FE_TONEAREST`, disabling fast-math,
or passing seven numeric samples is insufficient. Guest math must not silently
be replaced by the host system's `libm`.

## Sequence

1. Capture the current dirty comparison baseline and pin upstream. Build the
   isolated interpreter/AOT qualification probes. **Initial run: 13/17 pass.
   Patched follow-up: 33/33 pass in classic/fast Release and sanitizer lanes;
   the three original failure cases are fixed locally, not full acceptance.**
2. Map public APIs/options to WAMR mechanisms and close blocking upstream gaps
   in structured errors, pre-start cancellation, numeric behavior and limits.
   **Layout implemented. Deferred guest initialization and private stop-token
   wiring now cover cancellation before/during start.** Keep original ABI/import
   signatures and host policy ownership. Typed traps, limits and full public
   invocation scope remain open.
3. Implement the replacement behind existing Forge leaves; compile unchanged
   high-level consumers and mechanically migrate only unavoidable low-level
   binding points. Retain the old implementation only as a test oracle until
   the replacement passes; never silently execute it in the WAMR lane.
   **Started: private engine ownership/call implementation is used by the probe.
   Shared public bridge source is wired; the focused local classic capacity/depth
   target passed a historical 65f974b snapshot. Its later public checkpoint
   retains unit/parser and invocation/SDK trap failures. Fresh usage-1 tests and
   complete consumer/installed acceptance remain pending.**
4. Adapt the isolated compiler session/cache to WAMR AOT with explicit identity.
   Validate crash, corruption, cancellation, bounded resources and shutdown.
5. Run full differential/cross-platform tests, original contract corpus,
   downstream transactions and package/SDK tests. Independently review the
   final change before any default switch or release.

## Initial Experiment

The standalone [qualification project](../../tests/vm_wamr/README.md) deliberately
does not add a production library, switch a product pin or replace public API.
It uses the upstream loader and allocator directly to establish actual
integration gaps before designing the replacement bindings.

The probe's `probe.*` imports are test-only functions, not a second contract
intrinsic registry. Its small C fixture tests vanilla Clang-produced WASM;
it is not the Forge SDK or donor contract acceptance suite.

Upstream source notes and version selection are in the
[donor note](../donors/forge-wamr-v1.md). The
[API and option map](../donors/forge-wamr-api-map-v1.md) names the compatibility
work, including coupling in the pending OC API. The
[initial results](../donors/forge-wamr-qualification-v1.md) record the exact
failure cases and evidence identity. Runtime test reports are local artifacts
under `build/`; missing reports must never be interpreted as successful tests.

The [patched results](../donors/forge-wamr-patched-qualification-v1.md) record
the numeric/cancellation patch, sanitizer alignment fix, private implementation
work and still-open acceptance gates. Their historical full-capacity requirement
is superseded only by the explicit approved capacity decision above; the reports
and baseline remain immutable.
The [boundary and initialization follow-up](../donors/forge-wamr-initialization-qualification-v1.md)
records the newer patch-set identity and library-produced AOT checks separately.
The [host-binding follow-up](../donors/forge-wamr-bindings-qualification-v1.md)
records module-local bindings, partial metadata admission, caller memory caps,
the shared memory/ABI build profile and the still-unintegrated OC session/cache.
The [instruction/depth follow-up](../donors/forge-wamr-limits-depth-qualification-v1.md)
records logical stack/structural metrics, interpreter/AOT call depth, the
exported-import ABI correction and the historical large-local capacity gap.
Its observed rejection remains true; the required disposition is now explicit
rejection under the approved bounded profile. Passing focused regressions is
not public backend or OC acceptance.
