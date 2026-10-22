# WAMR Compatibility Work Map

Status: compatibility work map, not an accepted public API implementation.
Private implementation progress and remaining gaps are recorded in the
[instruction/depth qualification](forge-wamr-limits-depth-qualification-v1.md), following the
[binding qualification](forge-wamr-bindings-qualification-v1.md) and the
[patched qualification](forge-wamr-patched-qualification-v1.md).
Comparison sources are identified by the
[baseline manifest](forge-wamr-baseline-v1.json), including pending OC work.
File names below refer to that snapshot; some do not yet exist on `dev`.
They are historical baseline owners, not the current physical layout. The
[library boundary review](../iterations/forge-vm-wasm-wamr-v1.md#library-boundary-review)
has approved the WASM family. `execution/wasm` is now `vm/wasm/execution`;
the new private compiler implementation lives in `vm/wasm/compiler`. The old
snapshot and manifest remain unchanged. See the
[migration map](../releases/unreleased-wasm-family-boundaries.md). Public
compiler/session adaptation and optional package wiring are now present;
source/configuration integration is not installed OC replacement acceptance.

The maintainer-approved [bounded deterministic profile](forge-wamr-public-profile-v1.md)
supersedes the historical full-capacity requirement only. The 65,536-local
fixture must be rejected equally by interpreter and AOT admission; it is not a
request to widen upstream counters. Required donor behavior, consumer APIs,
safety, numeric determinism, cancellation and isolation are not waived.
The [fork provenance](forge-wamr-fork-v1.md) identifies the shared runtime/compiler
source URL and accepted pin `66e6be6c626856758ada54ddac2fc81ae49f5648`;
old patch files are historical provenance, not current build inputs. Published
fork PR 1 contains 11 commits across 29 files (946 insertions, 339 deletions).
The reviewed final usage-0 follow-up passed 256/256 low-level checks plus one
Release layout probe. It is not full public or product acceptance. Common
usage-1 ABI is now enabled for the approved allocator-dispatcher implementation;
fresh full qualification waits for its provider freeze.

## Consumer Boundaries

| Existing owner | Contract to preserve | Replacement work |
| --- | --- | --- |
| `libraries/execution/wasm/compiled_module.cppm` under its public include directory | Constructor from WASM bytes and limits; `invoke` with input, limits and stop token; result bytes/console; `bytes()` | Replace private implementation. Keep synchronous ownership and fresh invocation state. No WAMR types or new product scheduler |
| `libraries/execution/wasm/limits.cppm` under its public include directory | Input/output/console budgets and invocation deadline | `compile_limits.vm` currently exposes `interpret::options`; keep these checks, not just the field names |
| `libraries/vm/wasm/interpret/.../backend.cppm` | Validation without running start; initialization; named calls; whole-invocation cancellation | WAMR load/instantiate/call is not a drop-in implementation of the current templated backend/context. Adapt inside Forge and compile real consumers before promising zero source changes |
| `interpret` host registration, `type_converter`, `execution_interface`, `argument_proxy` | Original import signatures, checked memory arguments, host context, nested calls and host exceptions | Keep VM host-call mechanics generic; SDK adapters generate registrations from the canonical registry. Guest offsets cannot be cast to host pointers without bounds/lifetime checks. Never unwind a C++ exception through WAMR C frames |
| Cached interpreter contexts and watchdog consumers | Sequential reuse of one caller allocator; three-argument absolute-deadline `timed_run` and `interrupt_options` | Actual consumer gates identified by the coordinator: qualify borrow A-to-B handoff and port the existing watchdog surface. Neither break is approved; separate owners are handling them. Private system-provider tests do not close this gate |
| `interpret::wasm_allocator`, operand/value types | Stable value transport and correctly scoped linear memory | Approved usage-1 plan: public instances exclusively borrow the existing caller allocator/mapping via one process-lifetime usage dispatcher and scoped owners. No second buffer, moving growth or public system-provider fallback. Implementation/freeze and actual growth/lifetime/ownership tests pending; private system-provider passes cannot qualify this contract |
| `optimizing::compile_request`, `compile_configuration`, `target_profile` | Immutable WASM/profile/import identity and bounded compilation | Include engine revision, numeric profile, target and WAMR options in a new artifact identity; reject old OC payloads and rebuild from WASM |
| `optimizing::compiler_session` | Existing scheduler, bounded queue/bytes/cache, child process deadline, cancel and shutdown | Reuse lifecycle owner; replace compiler protocol payload where needed. No parallel queue/cache/process manager |
| `optimizing::executor` | Start/shutdown, thread preparation/release, typed traps, host errors, per-call deadlines and stop tokens | Preserve owner/bindings/host APIs. Only old `artifact::entry()` / `entry_function` machine-entry API removal is explicitly approved; migration uses public executor invocation and WAMR's own ABI, no raw adapter/always-throw placeholder. Exact scope and pending implementation are in the release note |
| `tooling::wasm_compiler::compiler` | Compilation isolated from runtime and I/O workers | Reusable compilation belongs to `vm/wasm/compiler`; private source admission is qualified and root optional target/helper/exports configure. Public session/artifact and installed-package acceptance remain open. Tooling stays a command wrapper; runtime-only LLVM exclusion remains a required acceptance test |
| Guest SDK, ABI generator, validation and testhost | Clang/lld guest build, original imports and canonical ABI | Do not change contract format to accommodate the engine. Testhost/validator must eventually run the same accepted profile as production |

The implementation files are under each named library; public files are under
`include/forge/<library>/`. The table deliberately does not propose an umbrella
facade or a second family of public `wamr::*` interfaces.

The agreed shared mechanism is owned by `interpret`: `prepared_module` admits
original WASM and provides the source-plus-native path, and `runtime_context`
serves interpreter and AOT. The latter path must validate the original source
before native loading. `optimizing` retains its existing compiler-session/cache
and executor lifecycle while consuming this same neutral bridge; no second
WAMR instance, binding registry or memory owner is introduced. The agreed
source-plus-native overload is present in current source, not a completed
public integration claim. Compiler admission reuses `engine_limits` compiled
under the compiler's actual private ABI definitions.

The same source profile now bounds prospective 64-bit AOT instance metadata
by `max_instance_metadata_bytes = INT32_MAX`, including aligned header, memory,
global, table and extra-state extents. This native signed-offset bound is not
a memory-page cap. It needs fresh source-only exact/next-representable and
compiler/runtime tests; a declaration or loader success is not qualification.

## Execution Options

Every current `interpret::options` field is accounted for below. These are
requirements for the replacement, not claims that the raw WAMR probe enforces
them. Profile checks must operate on the same validated module as execution;
do not copy the EOS parser or add a hand-written second WASM decoder.

| Fields | Required treatment |
| --- | --- |
| `max_section_elements`, `max_type_section_elements`, `max_import_section_elements`, `max_function_section_elements`, `max_global_section_elements`, `max_export_section_elements`, `max_element_section_elements`, `max_data_section_elements` | Admission counts with the current fallback rules. WAMR's general validity checks do not establish these application-selected limits |
| `max_mutable_global_bytes`, `check_mutable_global_bytes`, `forbid_export_mutable_globals` | Count mutable state and enforce export policy before any guest execution |
| `max_table_elements`, `max_element_segment_elements`, `max_data_segment_bytes`, `max_linear_memory_init` | Validate declared limits and initialized contents, including segment boundaries |
| `max_pages` | Enforce caller/module/profile policy before allocation and growth. Revised common cap PROPOSAL: 65,535 = `UINT32_MAX / 65536` pages; 65,536 pages wrap callback bytes to zero and require typed early source/config refusal, not ambiguous operational failure. Keep genuine zero initial pages distinct. Previous private cases do not qualify this boundary; fresh exact/excess source, compiler/runtime, configuration and zero/growth tests required |
| `max_func_local_bytes`, `max_func_local_bytes_flags`, `check_func_local_bytes`, `max_local_sets`, `max_code_bytes` | Preserve logical accounting and caller budgets within the approved bounded profile; reject unsupported configurations explicitly, never silently clamp. Qualify 8/16 KiB exact boundaries. Native compiler frame size is not an equivalent measure |
| `max_nested_structures`, `max_control_depth`, `check_control_depth`, `max_legacy_nested_structures`, `check_legacy_nested_structures`, `max_br_table_elements` | Preserve configured structural limits. Classify any historical-compatibility deviation explicitly |
| `max_symbol_bytes`, `max_memory_offset`, `parse_custom_section_name` | Enforce parser policy without relying on upstream default sizes or silent custom-section skipping |
| `max_call_depth` | Logical call-depth limit in both modes. Native stack reservation alone is not equivalent |
| `allow_code_after_function_end` | Human-approved unsupported/deprecated malformed-parser permission: reject trailing instructions after the function end even when enabled; new profile tests expect rejection, historical evidence unchanged |
| `allow_invalid_empty_local_set` | Human-approved unsupported/deprecated malformed-parser permission: reject invalid types in empty locals groups even when enabled; new profile tests expect rejection, historical evidence unchanged |
| `allow_u32_limits_flags` | Human-approved unsupported/deprecated malformed-parser permission: reject reserved memory/table flags even when enabled; new profile tests expect rejection, historical evidence unchanged |
| `allow_zero_blocktype` | NOT covered by the three approvals. Its current admission-policy discrepancy remains open; do not claim retirement or silently ignore this option |

Other public modes need independently recorded dispositions. Validation-only
`null_backend` now uses non-executable upstream-validated preparation and shares
the single-result guard; focused source evidence is in the public compatibility
record. Its full fresh usage-1 consumer gate remains open. x86-64 `jit` and
`jit_profile` callers/profiling are not qualified or retired by an AOT result.

The private `engine_limits` implementation covers fourteen metadata metrics,
mutable-global export policy and eight instruction/frame metrics. Loader
observations retain logical operands separately from fast-interpreter physical
slots. All parameter/local/stack flag combinations use a per-function sum.
The new logical depth counter covers defined calls, imports and deferred start
in the classic/fast interpreters and library-produced AOT. A typed flag crosses
the C boundary; native stack bytes are not used as a substitute.

This is still not complete admission. The preserved historical comparison
demonstrated that the old public `execution::compiled_module` admitted a
65,536-local module rejected by WAMR's 16-bit local-cell limit. The maintainer
has now explicitly accepted narrowing this extreme capacity: the fixture is
retained as expected rejection, not removed or made to pass through EOS fallback.
Exact common caps and typed rejection at the public boundary remain integration
work. Early loader allocation bounds, remaining parser policy, public error
mapping and artifact-profile validation remain open. Existing option names do
not imply support beyond the versioned profile; unsupported settings require an
explicit rejection or individually approved migration.

The three named malformed-parser retirements are explicit approved policy, not
automatic green results for old permissive assertions. Preserve their original
evidence and donor inventory; add current-profile rejection regressions with
valid controls and equal interpreter/validator/compiler behavior. The separate
raw-entry API break is limited to the confirmed declarations in the
[release note](../releases/unreleased-wamr-bounded-profile.md#raw-oc-entry-api-break).
No `compiled_module`, host API, executor or session/cache removal is authorized.

The page predicate covers INITIAL logical pages. Source with a small initial
memory and declared maximum 65,536 or no explicit maximum remains valid, with
growth capped by the common/caller/module policy. Explicit configuration caps
65,536 through `UINT32_MAX - 1` get typed refusal; `UINT32_MAX` retains the
existing unbounded-sentinel meaning intersected with the profile. New private
source fixtures compare this admission with compiler output/refusal without
instantiating large memory; those additions are NOT_RUN because the four finite
standalone builds failed at upstream CLI AOT generation before runtime CTest.
The fresh compiler's four CTests pass separately. See the
[finite checkpoint](forge-wamr-public-profile-v1.md#finite-standalone-checkpoint)
for exact scope and the suspected usage-callback CLI ABI mismatch; no private
full-profile pass is claimed.

Focused qualification now checks combined parameters/locals/peak operands at
32 KiB and one representable i32 excess, plus 65,536-local rejection, in
classic/fast runtime lanes and the production compiler helper's original-source
admission. This is not every fast-offset/signature shape or public artifact
acceptance. These are historical usage-0 results. Shared
`capacity_profile::max_function_results = 1` is the accepted P1 rejection rule:
check every validated type before public signature projection in runtime,
validation-only preparation and compiler source admission. The public checkpoint
rejects a valid multi-result module; fresh compiler/AOT equality remains its own
gate. Other declared caps (module 16 MiB, function storage 32 KiB, depth/control
1024, revised pages 65,535 = 4 GiB minus 64 KiB) remain PROPOSAL. The earlier
65,536-page proposal was not qualified support and is withdrawn because its
byte count cannot fit the u32 usage callback. No callback/counter widening is
approved; the public owner's explicit refusal fix and fresh source-cap/compiler
consistency tests remain gates. See the
[current test scope and results](forge-wamr-public-profile-v1.md#profile-probe-results).
The coordinator additionally qualified a historical focused public classic interpreter
32 KiB/+4 and 8/16/32 KiB by depth 251/1024 cases. That local target pass does
not erase the later old-pin invocation/SDK trap failures or close public AOT,
live native frames or full resource/profile acceptance. Current usage-1 runs are
pending; the [rerun plan](forge-wamr-public-profile-v1.md#fresh-full-standalone-rerun-plan)
must not execute until the private allocator/provider and centralized compiler
profile are frozen and the compiler is rebuilt with matching identity.
Standalone linkage of `engine_memory.cpp` and the shared post-upstream runtime
helper is now registered; this source wiring is not a build/test pass. Reuse
owned completed standalone directories, not duplicate root builds; a preceding
usage-1 compiler identity does not cover the centralized profile change.

The initial private module-local bridge probes derive signatures from
`FORGE_CONTRACT_INTRINSICS` and exercise five SDK-style callbacks. Public backend
and execution adaptation is now present with focused local classic test evidence;
this does not qualify every registered function/converter or the AOT executor.
Host failures must preserve their original exception and precedence over racing
cancellation. The [neutral public contract](../../tests/vm_wasm/WAMR_COMPATIBILITY.md#neutral-trap-contract)
exports `guest_trap : interpreter` with a typed reason; base catches remain
valid and memory bounds retain `exceptions::memory`. Physical native/operand
stack and local allocation failures use operational `execution_unavailable`,
not logical-depth traps. Unknown/auxiliary-stack reasons fail closed in the SDK,
not as canonical guest outcomes. Precedence is host failure, cancellation,
dedicated logical-depth flag, then structured engine information; no message
parsing. The current fork provides that information, but complete public/AOT
mapping, poison/discard semantics and fresh usage-1 regression results remain
mandatory gates. Old missing-getter results are historical, not current proof.

## First Blocking Work

1. Specify and implement one deterministic numeric profile in interpreter and
   AOT, including the host-call boundary. NaN canonicalization alone does not
   fix host rounding, subnormal behavior, conversions or compiler folding.
   The tracked patch passes the local ARM64 subset; the full gate stays open.
2. Make cancellation effective for ordinary-memory AOT loops and for the whole
   start/apply interval. Existing cooperative checks are conditioned on shared
   memory in unpatched upstream. Ordinary-memory loop cancellation is now
   patched and tested. Deferred initialization now allows a stop token before
   start, during start and before a later export. Public whole-invocation
   deadlines, admission bounds and blocking-host interruption remain open.
   Do not change guest memory semantics to hide this gap.
3. Establish typed runtime failure transport and admission/limit enforcement
   without parsing error strings or retaining EOS implementation transitively.
4. Implement the Forge-owned bindings, then compile and execute actual SDK,
   validator, testhost and product consumers against the installed package.

The experiment is not accepted until the complete
[migration matrix](../iterations/forge-vm-wasm-wamr-v1.md) is closed.
