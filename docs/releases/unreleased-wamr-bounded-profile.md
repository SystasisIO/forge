# Unreleased: WAMR Profile And Scoped API Breaks

Status: maintainer-approved requirement change for the experimental WAMR
replacement, updated 2026-10-19. Public backend/AOT qualification and release acceptance
are unfinished. This note does not switch the default backend.
The [fork provenance](../donors/forge-wamr-fork-v1.md) records the immutable
runtime/compiler source and identity; patch files referenced by earlier reports
are retained as provenance only, not applied during builds.
The current published draft engine candidate is `69c107c`; the
[follow-up record](../donors/forge-wamr-dev-integration-20261018.md) distinguishes
implementation, review, publication and fresh qualification. Earlier reviewed usage-0 results
(256/256 plus one Release layout probe) do not qualify the new usage-1 allocator
profile or constitute public/product release acceptance.

The new candidate adds an internal, first-failure FP status without changing
the public C API or the existing AOT prefix. Its F6 local checkpoint passed
76/76 selected runtime tests and 5/5 installed compiler/AOT/session tests on
Darwin ARM64. Installed API-stream and interpreter/JIT consumers also ran with
exit 0. Spine passed three contract gates and one real-process bootstrap/read/
restart scenario with 293 assertions. The process scenario does not select AOT;
the separate token-parity gate does. These results belong to the frozen F6
integration source, not automatically to this publication branch rebased onto
`05f9edaf`. F6 runtime-OFF subsequently passed 14/14 selected runtime tests,
2/2 installed AOT/session tests and a direct interpreter smoke. Resolved runtime
dependencies contain no LLVM; the full-ON compiler helper is a separate process.
Publication-branch, wider Turbine, x86 and remote CI qualification remain
separate gates. Results from the prior `12d1f90` pin are historical, not transferable.

The separate publication source `7d0556d1` subsequently passed 55/55 local
Darwin ARM64 CTests: 53 AOT checks, the full 301-case unit suite and SDK execution.
This closes the observed nested-cancellation classification failure locally,
including an intermediate scope's own stop, without suppressing real host
failures. The seven prior WASM fixture hashes remain unchanged. See the
[publication checkpoint](../donors/forge-wamr-dev-integration-20261018.md#publication-checkpoint-2026-10-19)
for strict proof boundaries and actual CI toolchain failures. Fresh remote,
physical-provider, package and product gates remain open; this is not acceptance.

The publication reconciliation also retires unregistered EOS-OC compiler-test
scaffolding that depends on the approved-removed raw entry ABI. Its immutable
`05f9edaf` preimages and historical golden references remain available. The 47
current AOT checks are not a one-to-one replacement for every old parameterized
integer, memory, control-flow, raw-FP or benchmark oracle. Reconcile those
operational checks through the current public compiler/executor before claiming
complete AOT compatibility; deleting the obsolete scaffolding does not waive
that acceptance gap.

## Affected Surface

The maintainer subsequently approved changing preparation sizing from an exact
prospective `byte_size()` promise to an enforced conservative reservation ceiling.
Session preparation must reserve that ceiling before allocation, reject an
excess and refund unused storage after release; successful preparation is no
longer required to consume every reserved byte. The implementation is present
and the preserved pre-integration tree passed scoped reservation/session tests;
fresh integration, package and product qualification remain required.
Interpreter/AOT consumers remain LLVM-free; explicitly optional
LLVM JIT configurations may carry their own compiler dependency.

The replacement will not preserve the entire historical EOS-derived WASM input
capacity. The 65,536-i32-local fixture is intentionally outside the supported
profile and must be rejected before execution by both interpreter and AOT
source admission. Existing option names or broad numeric types do not promise
acceptance beyond the versioned profile. Unsupported configurations require
explicit rejection, not silent truncation, clamping or fallback to EOS.

This is a deliberate compatibility break in supported module capacity, not a
claim of full historical parity. Apart from the scoped API breaks below,
consumer APIs, required donor contracts,
determinism, safety, cancellation and isolation remain required. Exact common
caps and physical-stack provisioning are tracked in the
[public profile decision](../donors/forge-wamr-public-profile-v1.md), not inferred
from a small contract corpus or an upstream integer width.

P1 admission now requires `max_function_results = 1`: valid multi-result types
must be rejected before public signature projection in runtime, validator and
compiler paths. Other declared numeric caps remain PROPOSAL. The
[neutral public trap contract](../../tests/vm_wasm/WAMR_COMPATIBILITY.md#neutral-trap-contract)
keeps base `interpreter` catches valid and separates guest, memory and local
operational failures; unknown reasons are not canonical guest outcomes.

The revised `max_pages` PROPOSAL is 65,535 pages, derived from
`UINT32_MAX / 65536`: 4 GiB minus 64 KiB. The earlier 65,536-page proposal was
never qualified support; its byte count wraps to zero in the existing u32 usage
callback. Over-cap initial memory/unsupported configuration must receive typed refusal
before narrowing/allocation, without widening WAMR counters or callbacks. Valid
zero-initial memory is a separate supported semantic requirement, not that
overflow case. Fresh source-boundary, compiler/runtime-consistency and public
configuration/zero/growth tests remain pending; this note claims no new pass.
The source cap checks initial pages, not a declared maximum of 65,536 or an
absent maximum; growth remains profile-bounded. Explicit configured caps
65,536 through `UINT32_MAX - 1` are refused, while the existing `UINT32_MAX`
sentinel stays bounded by the profile.

The approved allocator plan uses the existing caller `wasm_allocator` as the
sole public linear-memory mapping through a usage dispatcher and exclusive
instance ownership, without a second buffer. Idle reset/handoff must retire the
old instance before page mutation; active execution includes host calls and
argument conversion. The common usage-1 flag is enabled, and idle retirement
and active-use exclusion are implemented. Scoped public tests on the preserved
tree do not replace fresh full integration/product qualification.

## Retired Parser Permissions

The maintainer explicitly approved retiring these three historical permissions
for malformed WASM in the replacement profile:

- `allow_code_after_function_end`: instructions after the function end.
- `allow_invalid_empty_local_set`: invalid type in an empty locals group.
- `allow_u32_limits_flags`: reserved memory/table flags.

They are unsupported/deprecated permissions, not a promise to accept malformed
input when enabled. New profile tests must expect rejection, including with
these old flags enabled. Historical donor evidence/manifests remain unchanged;
fresh regression results must be recorded separately. Regenerate affected WASM
using a valid encoding instead of relying on these permissions. This approval
does NOT include `allow_zero_blocktype`. That option is preserved with explicit
numeric/legacy-zero modes in the existing loader, shared by runtime and compiler;
only the owned input copy is normalized. Fresh evidence is in the follow-up.

## Raw OC Entry API Break

Explicitly approved on 2026-10-16; source removal is confirmed by the AOT owner
and coordinator. Full runtime/package/release acceptance remains pending.
Confirmed affected declarations:

- `forge::vm::wasm::optimizing::artifact::entry(std::uint32_t) const` in
  [artifact.cppm](../../libraries/vm/wasm/optimizing/include/forge/vm/wasm/optimizing/artifact.cppm).
- `forge::vm::wasm::optimizing::entry_function`, the old
  `void (*)(invocation_abi*, const interpret::expr_value*, interpret::expr_value*) noexcept`
  machine-entry alias in
  [invocation_abi.cppm](../../libraries/vm/wasm/optimizing/include/forge/vm/wasm/optimizing/invocation_abi.cppm).
- In that same module, the AOT owner confirms removal of the dependent
  `native_function::entry` and `native_function::reserved` fields,
  `host_call_callback`, `grow_memory_callback`,
  `numeric_callback`, `poll_callback`, `trap_callback`, `invocation_callbacks`,
  `table_entry` and the old 168-byte `invocation_abi` machine context.

The module name remains: it owns live `trap_reason` and `native_function`
metadata, including `stack_bytes` and `type_index`; this is not module or metadata
API retirement. The current target identity is
`current_host_abi_version = 3` / `target_profile::host_abi_version = 3`, with
`codegen_version = 5`. Target-profile fields and legacy serialized payload
sections remain; this task does not authorize a separate wire-format rewrite.
Rebuild native artifacts under the new checked identity, not by relabeling old
machine code. The owner reports compiled negative API assertions passing;
complete runtime/consumer/release qualification remains pending.

This is an intentional public C++ source/API break, not a compatibility alias,
raw-entry adapter or always-throw placeholder. The coordinator's search found
no product consumers of the Forge raw-entry API. Its Spring donor trace found
internal `apply_func` calls in `executor.cpp` at lines 249/265, not consumers of
these Forge declarations; WAMR supplies its own internal native-call ABI.
This scoped decision does not remove `compiled_module`, host APIs, executor,
compiler session or cache ownership, and does not itself change persisted state.

Migrate pointer extraction/calls to the existing public
[executor](../../libraries/vm/wasm/optimizing/include/forge/vm/wasm/optimizing/executor.cppm):
prepare/provenance-check the artifact, create `executor::bind<Registered>`, then
use `executor::invoke` with the function index, arguments, caller allocator,
deadline and stop tokens under its existing start/thread/shutdown lifecycle.
Keep host registration and failure/cancellation semantics. Do not cast a WAMR
entry address or `native_code()` bytes to the removed uniform function type.
Use `artifact::functions()` for supported metadata inspection, not entry-pointer
extraction. Runtime/package/consumer acceptance of this migration remains
pending; additional removals require their own explicit scope.

The earlier finite standalone attempt had compiler CTests 4/4 PASS but all four
runtime builds fail at upstream CLI AOT fixture generation; runtime CTests were
not run. Fresh root AOT 32/33 and fail-closed session checks do not establish the
full 32 KiB physical envelope or successful session/cache acceptance. See the
[dated scoped results](../donors/forge-wamr-public-profile-v1.md#finite-standalone-checkpoint).
Cached-context allocator handoff and actual consumer-shaped watchdog APIs were
also open at that checkpoint, not additional approved compatibility breaks.
The current follow-up closes their selected public tests, not the complete
installed/product workflow or exact session reservation.
These earlier results are retained as history. The
[current experiment checkpoint](../iterations/forge-vm-wasm-wamr-v1.md#current-qualification-checkpoint)
records the subsequent 45/45 AOT run, including scoped session/cache tests on
the preserved tree. It does not qualify a different source tree, installed
components or full product processes.

## Legacy Parser And Allocator API Break

Approved on 2026-10-17; implementation and fresh qualification are tracked in
the replacement plan. The approved public source removals are:

- `forge::vm::wasm::interpret::interpreter::parser`;
- `forge::vm::wasm::interpret::null_backend::parser` and `context`;
- `forge::vm::wasm::interpret::jit_allocator`;
- `forge::vm::wasm::interpret::growable_allocator`.

Callers should load and inspect WASM using the existing public backend and
`prepared_module` interfaces, including the validation-only backend. Their
validation, import/export signatures and capacity checks remain supported.
Machine-code allocation belongs to the engine under Forge's resource budgets;
consumers do not allocate executable pages using the retired donor classes.
Preserve `interpreter::context` and caller-owned `wasm_allocator` for execution
context reuse and linear memory. No public WAMR C structures replace these APIs.
This scope does not automatically remove live host-conversion or value types.

The removed parser tree has the following dependent source changes in
`forge.vm.wasm.interpret.types`:

- `guarded_vector`, `resizable_limits`, `init_expr`, `global_type`,
  `global_variable`, `table_type`, `memory_type`, `import_type`, `import_entry`,
  `export_entry`, `elem_segment`, `local_entry`, `function_body`, `data_segment`,
  `name_assoc`, `indirect_name_assoc` and `name_section` are removed.
- `module` no longer owns the parsed-tree fields `allocator`, `start`, `types`,
  `imports`, `functions`, `tables`, `memories`, `globals`, `exports`, `elements`,
  `code`, `data`, `names`, `import_functions`, `type_aliases`, `fast_functions`,
  `maximum_stack`, `error` and `jit_mod`.
- Its parser/codegen methods `make_jit_module`, `finalize`, `normalize_types`,
  `get_imported_functions_size`, `get_imported_functions_size_impl`,
  `get_functions_size`, `get_functions_total`, `get_function_pc`, `get_opcode`,
  `get_function_locals_size`, `get_function_type`, `get_exported_function` and
  `get_exported_function_impl` are removed.
- `module::jit_mod_t` loses `jit_export_entry`, `jit_data_segment`, its fields
  `types`, `imports`, `functions`, `table`, `memories`, `globals`, `exports`,
  `jit_code_offset`, `data`, `import_functions`, and methods `get_function_type`
  and `get_imported_functions_size`.
- `func_type::param_types` uses `std::vector<value_type>` instead of the retired
  `guarded_vector<value_type>`.

`backend::get_module()` returns the existing `prepared_module`, not this old
parser tree. Migrate supported inspection to its validated metadata and
`validate()`; there is no replacement raw-opcode/tree-mutation API.
`module::jit_mod_t::jit_func_type`, `jit_import_type` and `jit_import_entry`
remain because public prepared-module metadata uses their names. The independent
`expr_value` union and `func_type` value contract remain available.

In `forge.vm.wasm.interpret.host_function`, the old
`registered_host_functions<...>::resolve(module&)` and
`resolve_impl<Module>(Module&)` are removed with that parser tree. Host
registration, conversion, invocation, `resolve_import`, and `resolve(Module&)`
for prepared metadata exposing `function_imports()` remain. Migrate old
resolution calls to the prepared-module overload; do not expose WAMR structures.
These are dependent source breaks, not deletion of the live host ABI.

The low-level `invoke_with_signal_handler` overloads taking a
`growable_allocator*` or `growable_allocator&` retire with that allocator.
The range-based helper takes `std::span<std::byte>` instead; pass an explicitly
owned range only when using the signal helper directly. A null-code-range call
remains supported through `nullptr`, so callers without an executable arena
need no migration. WAMR cancellation does not use this helper to disable code
pages, and products must not extract or manage WAMR executable pages themselves.

`jit_profile` is not retired. The candidate implements instruction-level
profiling through WAMR/LLVM, including native-address to original WASM-offset
mapping and an owning immutable observer. Public runtime, sampling and package
qualification remain acceptance gates. Ordinary JIT execution and function-level
timing alone do not establish equivalent behavior. Callers must stop and join
sampling before moving or destroying an observer; native maps cannot be relocated.

## Native Signal API Break

Explicitly approved on 2026-10-18. This is an intentional public C++ source/API
break in `forge::vm::wasm::interpret`, not a source-compatible implementation
change. The following declarations existed on `origin/dev` at
`78a40fd86dc8f88593c2bcae5023ef4e7db840fe` and are already absent from the
current frozen WAMR candidate. This decision approves only the listed removals.

Removed types:

- `native_signal_fault`;
- `native_signal_ranges`;
- `signal_boundary_stage`;
- `signal_boundary_status`;
- `native_signal_result`;
- `signal_recovery_entry`.

Removed functions:

- `prepare_signal_recovery_thread`;
- `release_signal_recovery_thread`;
- `invoke_native_with_signals`;
- `leave_native_with_signals`;
- `invoke_without_signal_recovery`.

Migrate WASM invocation ownership to the existing public `backend`,
`prepared_module::context` / `runtime_context`, or, for optimized artifacts,
`forge::vm::wasm::optimizing::executor`. Use their real WAMR guest entry/exit,
trap and host-exception transport, cancellation and thread lifecycle; keep
initialization/start inside the bounded invocation. Remove the caller-managed
native signal boundary rather than casting WAMR addresses to the retired entry
type or forwarding through an unrelated recovery mechanism. These are semantic
migration paths for WASM execution, not replacements for arbitrary native
callback execution or promises of the old signatures.

Do not restore the listed declarations as stubs, casts or compatibility wrappers.
This approval does not retire `watchdog`, the retained
`invoke_with_signal_handler` helpers (including `nullptr` code-range calls), or
numeric execution and floating-point environment utility APIs. Their preservation
and qualification remain required; no broader API retirement or all-platform
transport acceptance is implied. This note changes no compiled source or engine pin.

## Retained Numeric And FP APIs

The scoped removals above do not retire the existing numeric and FP utility
APIs in `forge::vm::wasm::interpret`. Their owning imports remain:

```cpp
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.floating_point_environment;
```

The first exposes
`expr_value execute_numeric(opcodes, std::span<const expr_value>)`; the second
owns the noncopyable/nonmovable `floating_point_environment` guard. The guard
saves/restores only the four SoftFloat environment fields, including nested and
C++ exceptional exit. It neither changes hardware FP state nor replaces WAMR's
guest FP or trap boundary. C definitions and C/C++ consumers must share the
restored `THREAD_LOCAL=__thread` ABI, including installed target requirements;
rebuild/reinstall rather than mixing old non-TLS archives with TLS declarations.

The numeric restoration uses one fixed WAMR module for exactly 62
FP/comparison/conversion operations and creates a fresh interpreter context per
call. It uses the shared WAMR numeric/FP implementation, not an EOS visitor or a
parallel interpreter, and leaves SoftFloat state unchanged. Arithmetic NaNs use
the approved canonical WAMR values; reinterpretation and raw sign/bit operations
preserve their specified payload/signed-zero behavior. This does not promise
historical EOS arithmetic-NaN payload parity, hard-real-time latency,
allocation-free calls or the old SoftFloat primitive's performance.

Status: numeric v2 and FP/TLS APIs are restored in source. Independent numeric
review is CLEAR, with the P2 finding closed. The pre-legacy-fix R5 full ON checkpoint
passed 72/72 selected CTests on Darwin ARM64 in 74.27 seconds, with zero
failures/errors/skips/disabled tests and no expected-failure masking. Coverage
includes core, interpreter/JIT, nine public-profile cases and 47 AOT/session/
cleanup registrations; helper/session checks select the current build-tree
helper. Unit and spec report 288 and 4856 cases, not assertion counts; all 290
source files matched that checkpoint's freeze. This selected matrix was not the
entire root suite or all-platform/product acceptance.

The earlier LLVM-free interpreter-only run remains a separate 10/10 PASS
checkpoint (165 unit / 2428 spec cases). The later F6 ON matrix passes 76/76
selected tests, including the V3 recovery and new normal-exit/FP-failure gates;
its installed AOT/session selection passes 5/5. The matching Spine checkpoint
passes 3/3 contract gates and the selected real-process case, not the complete
Spine suite. F6 runtime-OFF passes 14/14 selected tests and 2/2 installed
AOT/session tests, plus its direct interpreter smoke; resolved runtime
dependencies exclude LLVM. Linux/x86 and remote CI remain unqualified.
Earlier package/helper results are not silently
relabelled as F6 evidence. See the
[owning README](../../libraries/vm/wasm/interpret/README.md#retained-numeric-and-fp-apis)
for the exact component and lifecycle boundaries.

## Legacy Timed-Run Compatibility

Status: V3 is applied and qualified within the frozen F6 local ON checkpoint.
Source review is P1/P2 CLEAR; the 76-test runtime matrix, installed package and
unchanged three-gate Spine smoke passed. The selected Spine process also passed.
The separate F6 runtime-OFF checks also passed. Neither checkpoint is complete
product or cross-platform release acceptance.

The legacy timed-run correction restores the f6 contract after an unintended
migration regression; it is not a newly approved API break. Ordinary
`timed_run` remains a budget guard with caller-managed VM lifetime, not an
implicit reset or host/application/DB rollback. After a failed legacy scope
closes, reinitialize inside a new bounded scope before guest reuse: this retires
the old VM/memory borrow and creates fresh globals, memory and host association,
without destroying the caller-owned host or undoing its external effects.

Strict `with_invocation` host/native-failure poisoning is unchanged. The first
host failure recorded by an active scope survives raw context reset/clear and
reinitialization and outranks racing interruption. A closed scope's old failure
must not override a later pre-canceled or empty scope. This correction adds no
public WAMR types or application error policy; installed and product reruns
remain required for the corrected snapshot.

## Migration

Before adopting the replacement, validate original WASM and caller budgets
against the final versioned profile in both interpreter and compiler paths.
Rebuild modules outside that profile with smaller live frames/signatures or
keep them on their existing explicitly selected runtime until migrated. There
is no automatic fallback and no widening of upstream counters.

Invalidate old OC and mismatched WAMR AOT caches and regenerate native artifacts
from original WASM using the accepted engine/profile/target identity. Do not
reinterpret old native payloads or reset persisted contract state. Preserve
module/profile identity consistently across nodes before activation.

Source/runtime-only installation, public SDK and host bindings, required donor
execution, AOT session/cache lifecycle, cross-platform determinism, heavy live
native frames and product effect rollback remain release gates. See the
[acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md#acceptance-matrix).
