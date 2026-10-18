# Public WAMR Compatibility Checkpoint

This is an experimental migration record, not approval to retire public APIs.

## Current Follow-Up

The [2026-10-18 follow-up](../../docs/donors/forge-wamr-dev-integration-20261018.md)
tracks draft candidate `12d1f90`, public instruction profiling, length-aware
export names and current development prerequisites. Its fresh runtime/package/
product qualification is in progress. The approved API breaks and bounded
profile are described in the [release note](../../docs/releases/unreleased-wamr-bounded-profile.md).
The following dated sections preserve earlier assessments, including proposals
since implemented and failures since addressed; they are not current acceptance.

## Preserved 2026-10-16 Follow-Up

The [2026-10-16 follow-up](../../docs/donors/forge-wamr-follow-up-20261016.md)
tracks the new clean `75e8f91` source pin, allocator handoff/argument-conversion
repair, block-type policy parity and fresh tests. Sections below preserve the
earlier `66e6be6` and preceding checkpoints; their failures are not rewritten as
passes. Exact session/cache preparation remains a separate gate.

## Primary Backend Test Migration

The primary backend migration uses WAMR's `runtime_backend` and
`runtime_context`; deriving a selector from `interpreter` no longer selects the
removed EOS-derived implementation. Invocation tests select interpreter/JIT
coverage with `FORGE_VM_WASM_INTERPRET_HAS_JIT`, subject to the existing
`FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY` override, not host architecture.
Independent instances are checked through their actual `prepared_module`
identity. The removed backend's `module::allocator.get_code_start()` is not a
WAMR code identity; `native_code()` currently exposes AOT mappings only, not
LLVM JIT mappings, so an empty JIT span must not be compared as a code address.

The following old-backend-only tests are explicitly retired, not passing WAMR
tests and not replaced by an unrelated allocator exercise:

- `invocation_restore_failure_outranks_guest_and_host_errors_and_forbids_reuse`
  unmapped the old backend's code pages during `timed_run` and expected a
  subsequent `mprotect` restoration failure to outrank guest/host errors and
  poison the backend. WAMR cancellation terminates execution through its instance
  API; it does not revoke and restore those old allocator code-page permissions.
- `watchdog_disable_failure_is_fail_fast_not_a_timeout_or_unbounded_wait`
  unmapped the same code pages before watchdog expiry and expected
  `std::terminate` (child exit 86) when the callback's `mprotect(PROT_NONE)` failed.
  That callback and its terminate contract belonged to the removed backend.

Their `legacy_code_protection` selector and `expires_then_unmaps` helper are
removed with them. Git history retains the historical assertions. The standalone
`code_access_restoration_reports_real_mapping_failure` case remains unchanged
apart from an explanatory comment: `growable_allocator` still exposes
`enable_code`/`disable_code`, and this case tests only that allocator API.

Current replacement-mechanism gates, with their actual scope, are:

- In [invocation_scope_tests.cpp](invocation_scope_tests.cpp),
  `invocation_deadline_interrupts_start_and_export_and_allows_reuse`,
  `invocation_cancellation_interrupts_start_and_export_and_allows_reuse`, and
  `invocation_independent_compiled_instances_isolate_cancellation_and_deadline`
  exercise WAMR interruption, fresh reuse and neighboring-instance isolation.
  `invocation_host_failure_is_not_hidden_by_cancellation_or_body_catch` retains
  host-failure precedence; this is not a code-restoration-error test.
- [consumer_api_tests.cpp](../vm_wasm_execution/consumer_api_tests.cpp)'s
  `typed_external_stop_interrupts_actual_guest_and_allows_fresh_reuse` exercises
  the absolute-deadline/external-stop API on actual guest execution.
- The private `wamr_load_budget_real_loader` gate in
  [load_budget/CMakeLists.txt](../vm_wasm_execution/load_budget/CMakeLists.txt)
  runs [load_budget_probe.cpp](../vm_wasm_execution/load_budget_probe.cpp)'s
  actual source/AOT loader fault sweeps: native mapping denial, protection
  denial, cancellation and unmap failure retain typed reasons and bounded
  ownership; a later valid load checks recovery and thread protection state.
  This is native preparation/cleanup evidence, not a watchdog page-revocation
  test or qualification of LLVM JIT mappings.

The former forced-POPCNT-fallback selector and its architecture-conditioned
internal-target registration are removed. The historical filename
[x86_64_regression_tests.cpp](x86_64_regression_tests.cpp) now contains the neutral
public `jit_executes_integer_popcount` case in `test_forge_vm_wasm_invocation`.
It uses `wasm::jit`, checks native preparation metadata and i32/i64 zero,
single-bit, all-bit and mixed-bit results. Its registration is guarded by
`HAS_JIT` and the interpreter-only override on every supported host. It makes no
claim to disable a CPU feature or force a particular LLVM instruction path.

This bounded test migration has **NOT_RUN** build/runtime status: no build slot
was held, and no CMake configuration, compilation or test execution was run.
The gates above are source references, not fresh passes. In particular a
JIT-disabled run does not qualify JIT execution, cancellation or native mapping
denial. Public JIT preparation must satisfy its own allocation bounds before a
JIT-enabled matrix can pass; these tests must not hide a fail-closed rejection by
silently running the interpreter instead.

## Earlier Consumer Blocker And Absolute API

The earlier allocator freeze is not a consumer-compatibility acceptance.
Read-only inspection of the real consumer's `execute_cached_apply` confirms that
it keeps prepared backends and external contexts alive between actions, shares
one controller allocator, and calls only `allocator.reset()` after each action.
The current lifetime-long borrow therefore rejects cached B after cached A.
This is a P1 regression, not an approved policy change and not waived by the
allocator plan or capacity narrowing.

`consumer_api_tests.cpp` now compiles the actual public call shape: an externally
owned `Implementation::context<functions>`, `initialize_globals()`, backend
context/allocator attachment, and
`timed_run(steady_clock::time_point, interrupt_options, work)` around initialize
and `apply(i64,i64,i64)`. The runtime overload uses the existing watchdog's
admission gate before Work and joins all callback delivery before inspecting
context failure or mapping the sticky deadline/cancel/external reason. Host
failure remains dominant. The existing `interrupted = 4010008` contract matches
the current b40 public API. No second timer or interruption mechanism was added.

Fresh checks in the same warm build:

- `consumer-api-focused.log`: four absolute-API cases, 34/34 assertions pass;
  `consumer-api-repeats.log`: 20/20 repetitions pass. They cover pre-entry
  priority, actual guest external interruption and reuse, host-failure poison,
  same-module typed nesting and actual Work exceptions.
- `watchdog-absolute-integrated.log`: seven watchdog cases, 73/73 assertions
  pass after registration in the invocation target. The watchdog implementation
  and those tests belong to the separate watchdog developer.
- `consumer-api-ctest.log`: execution, engine memory and invocation pass;
  the new consumer target fails its ordinary cached A-reset-B-reset-A case.
  B does not run and the host counter is 2 instead of 3. This gate deliberately
  expects success, not an approved `busy` result.
- `cached-consumer-runtime.log`: an independent compiled consumer-shaped probe
  reproduces the failure. Explicit context retirement before memory reset is
  a passing control, not a required downstream workaround.
- `timing-allocation.log`: 32 injected stop-state allocation failures reject
  before Work, each followed by successful reuse of the same backend. The stop
  source and restore guard are now constructed before publishing either timing
  TLS or the borrowed stop-source pointer; allocation failure cannot strand TLS.
- `consumer-api-final-ctest.log`: the final bounded five-target selection has
  four passes (execution, engine memory, invocation, timing allocation) and one
  failure (consumer API, solely the cached handoff case with two failed checks).
  `consumer-api-final-build.log` records the successful incremental `-j2` build.
  This is the source freeze for the current slice, not a green migration gate.

The proposed neutral fix is a per-allocator synchronized owner-retirement
protocol. External reset must retire an idle instance before changing the
mapping; active guest/host execution must reject reset without mutation. Initial
reset by the current owner must be distinct to avoid recursive teardown.
Registration, callback lifetime and destruction must synchronize so no cached
context can access a stale instance. Simply clearing the borrow flag is unsafe.
Fresh initialization must also preserve the ordinary multi-context handoff
contract exercised by the eight unchanged donor cases, not silently waive them.
This protocol is an unreviewed direction, not an implemented or frozen API. It
requires its own reset/handoff, active-reset, concurrent-destruction and
failure-path tests before the next allocator change is accepted.
No second mapping, cache or product-specific lifetime hook is proposed.

Full downstream compilation and x86 JIT absolute-API qualification are not
established by these interpreter-shaped tests. The full public consumer gate
remains blocked by allocator handoff.

## Current Allocator And Shared-Profile Freeze

The current build uses WAMR `66e6be6c626856758ada54ddac2fc81ae49f5648`
and common feature-profile SHA-256
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`,
with usage-aware allocation enabled. `engine_exception` requires the pinned
structured getter directly; no text matching or compatibility fallback remains.

The public allocator is now the actual linear-memory mapping, not a checked
but ignored pointer. One process-lifetime dispatcher scopes each instance owner
around instantiate/call/teardown, including failed initialization. Its exclusive
borrow rejects a second live instance; nested and parallel scopes restore their
owners. No second linear buffer or public system fallback is used. C callbacks
capture failures without C++ unwinding. Physical grow failure terminates execution
and blocks a subsequent host import before its effects; logical maximum failure
still returns `-1`. Private system storage is zeroed on allocation and growth,
including reused allocations.

The revised version-1 proposal has `max_pages = UINT32_MAX / 65536 = 65535`,
or 4,294,901,760 bytes. Initial 65536-page declarations and explicit caller caps
65536 through `UINT32_MAX - 1` reject before allocation. The existing
`UINT32_MAX` sentinel applies the common cap. An omitted declared maximum remains
valid source, with growth bounded by the profile. Valid zero initial memory and
first growth are tested separately from an unrepresentable 4-GiB callback.
No callback or counter was widened; no multi-GiB allocation is claimed tested.

`max_instance_metadata_bytes = INT32_MAX` also bounds prospective native layout.
The shared observation uses upstream instance-header, memory and numeric-global
sizes, aligned table strides from `wasm_table_instance_size` and the instance
extra structure. Runtime/validation/compiler share this admission check. Tests
observe adjacent accepted/rejected table extents without instantiating a table.
The remaining constants are module 16 MiB, function storage 32 KiB including
parameters/locals/logical operand peak, one result, depth/control depth 1024.
This is not complete classic/fast/AOT physical qualification.

Fresh checks in the existing `build/wamr-public-sidecar`, Release, `-j2`:

- `allocator66-freeze-ctest.log`: execution, invocation, prepared validation and
  engine memory targets pass, 4/4. Structured traps close the prior invocation
  failure on the old pin.
- `allocator66-execution.log`: 27/27 cases, 244/244 assertions. This includes ten
  allocator cases with 83 assertions, source/configuration page boundaries,
  signed metadata admission, real failed `mprotect` before host effects, reset,
  initialization failure, cancellation, nested/parallel ownership and reuse.
- `allocator66-system-memory.log`: 4/4 cases, 130/130 assertions, including
  recycled initial storage, grown tails, zero-initial first growth, actual private
  instances and upstream-derived metadata boundary observations.
- `allocator66-unit.log`: unchanged historical donor suite is not green:
  156/175 cases, 767/794 assertions. Its 27 failures are the previous 19 plus
  eight allocator-sharing failures, not a successful full-regression result.

The eight new `busy` failures are `max_linear_memory_init` lines 100/111/121,
`max_memory_offset` lines 80/88/104, `max_section_elements` line 152, and
`instantiation` line 21. They keep multiple initialized backends alive on the
same allocator. This conflicts with the new exclusive-instance borrow and is
now a confirmed ordinary consumer regression, not an approved policy difference.
Their assertions remain unchanged; the new cached-consumer gate requires success.

The maintainer explicitly approved dropping the three malformed permissions.
New profile tests require malformed modules to reject with either requested
flag value, while a valid `compatibility_options` consumer still passes. The
six old permissive assertions remain historical evidence, not current promises.
The twelve blocktype-zero failures are separately unresolved and not waived.
Actual downstream compilation, installed consumers, full corpus, x86 JIT,
fast-interpreter and fresh AOT allocator/physical-stack matrices remain separate
gates. The runtime-only probe links libc++/libSystem, not LLVM.

## Historical Checkpoint

The following reference run uses WAMR `65f974bebba846e14e026552aaaa0b898b6f9498`.
The full unit executable ran 175 cases and reported 28 assertion/fatal failures;
that number is not the number of failed test cases. Donor bodies and their
assertions remain unchanged.

## Classification Of The 28 Failures

| Group | Failing check or case | Failures | Disposition |
|---|---|---:|---|
| A: ordinary API | `port_regression_tests_line_527` | 1 | Active table segment bounds must report the historical interpreter category, not memory. |
| A: ordinary API | `port_regression_tests_line_619` | 1 | Public indirect-call bounds and empty entries must report interpreter. |
| A: ordinary API | `port_regression_tests_line_746` | 1 | Attaching an allocator must not implicitly initialize an uninitialized backend. |
| A: ordinary API | `port_regression_tests_line_818` | 1 | Zero call depth retains the interpreter selector's vector-out-of-bounds category. |
| A: host lifetime | `preconditions_tests_line_90` | 2 | Independent direct calls must clear the previous host failure, without clearing invocation poison. |
| A: linking policy | `max_section_elements_tests_line_147` | 1 | Historical executing `backend<nullptr_t>` admits unused unlinked imports. Its explicit unlinked-execution mode is distinct from non-executable validation-only loading. |
| A: reentry | `reentry_tests_line_53` | 1 | Same-thread host-to-guest reentry must work; another thread cannot enter the same instance. |
| A: type safety | `backend_tests_line_16` | 1 | Reject a top-level i64 argument for an i32 parameter before reading the operand union. |
| B: narrowed capacity | `implementation_limits_tests_line_60`, final initialize | 1 | Requested depth 1,048,576 exceeds the common depth-1024 profile. Expected typed profile rejection; the original assertion remains. |
| C: parser permissions | `allow_code_after_function_end_tests_line_122/135` | 2 | WAMR rejects trailing instructions after the function end even when the historical permission is enabled. NOT_ACCEPTED compatibility scope. |
| C: parser permissions | `allow_invalid_empty_local_set_tests_line_56/66` | 2 | WAMR rejects an empty local group with invalid local value type even when permission is enabled. NOT_ACCEPTED compatibility scope. |
| C: parser permissions | `allow_u32_limits_flags_tests_line_96/112` | 2 | WAMR rejects reserved flags `0x3ffe/0x3fff` in place of memory/table flags `0/1`, which the historical permission accepts. NOT_ACCEPTED compatibility scope. |
| C: parser policy | `allow_zero_blocktype_tests_line_63/70/77/91` | 12 | WAMR interprets block signature byte `0` as type index zero, accepting these empty-signature fixtures while the historical option is absent/false. The historical extension treats zero like void `0x40`. This is an admission-policy bug, not an approved capacity difference. NOT_ACCEPTED. |

Group totals: A = 9, B = 1, C = 18. The exact parser differences are not waived
by the accepted capacity narrowing. Do not patch WAMR to accept invalid historical
bytecode or add a second parser solely to make these fixtures green. Preserve
the failures until policy is implemented or an explicit compatibility decision
is approved. Unsupported options must not silently be treated as supported.

## Bounded Follow-Up Results

The same 175-case unit executable now reports 19 failures: group B's single
extreme-depth check and group C's 18 parser differences. All nine original
group-A failures are fixed in the shared bridge. The eight affected original
cases also pass in a focused run with 33/33 assertions; no donor assertion was
changed or removed.

Seven public regression cases pass with 45/45 assertions. They cover the exact
32 KiB storage boundary, 8/16/32 KiB interpreter frames at depth 251/1024,
unlinked-import metadata and consumer context API, caller-unbounded page budgets
intersected with capacity, multi-result rejection, unexported function/import
table dispatch, and typed logical-depth/base-catch compatibility.

`capacity_profile::max_function_results = 1` is enforced on every validated
upstream function type by the common `engine_limits::check_profile` used by
runtime, validation-only preparation, and compiler admission. The public source
and validator test uses a valid `() -> (i32, i32)` module and expects profile
rejection before the function-type/result projection. A fresh compiler-side
gate is recorded by the compiler owner, not inferred from the runtime test.

The runtime-only public bridge probe additionally passes an AOT module with no
function exports: a private defined function and an unexported raw import are
called through table slots and numeric function indices over three fresh
contexts. Indexed AOT dispatch uses upstream `aot_get_function_instance`, not
export-only lookup or a duplicated invocation implementation. The test compiler
is the separately built `65f974b` helper; the runtime probe links no LLVM.

The four-target CTest selection is not green: prepared validation passes; unit
has the 19 documented failures; invocation has one pending structured-trap
classification failure. SDK execution now also has one pending trap test on
this old pin, because unknown traps are deliberately operational failures rather
than false canonical guest traps. All five selected targets compile. Full
spec/corpus, installed consumer execution, and actual downstream compilation
remain unqualified by this checkpoint.

## Neutral Trap Contract

`forge.vm.wasm.interpret.exceptions` exports `guest_trap : interpreter`, with a
public `guest_trap_kind kind` field. Values are `unknown`, `unreachable`,
`table_bounds`, `indirect_type`, `integer_divide_by_zero`, `integer_overflow`,
`invalid_conversion`, `logical_depth`, and `auxiliary_stack`. Existing
`catch(interpreter&)` remains valid. Memory bounds retain `exceptions::memory`;
native/operand physical stack and local allocation failures use
`execution_unavailable` rather than logical-depth traps. Unknown and auxiliary
stack reasons must not be mapped to canonical guest outcomes without an
explicit supported rule. SDK execution fails closed for both.

Host failure, cancellation, the dedicated logical-depth flag, and structured
engine exception information are checked in that order. No message parsing is
used. WAMR `65f974b` lacks the agreed getter, so its information is explicitly
unknown; the temporary compatibility branch must be removed when the reviewed
getter pin lands. Known numeric/table/unreachable classifications and their
AOT mapping are not accepted merely because the enum exists.

## Additional Consumer Gates

Validation-only `null_backend` uses WAMR `LoadArgs::no_resolve`; it exposes
function-import names and neutral function types without fake host functions.
Its prepared module is explicitly non-executable. The focused public regression
also compiles the consumer's `Implementation::context<functions>` constructor
and `initialize_globals()` surface. An executing `backend<nullptr_t>` preserves
unused unlinked function imports through a separate WAMR `no_resolve` mode;
calling an unlinked function traps. A supplied host registry and the generic
prepared-module default still resolve imports strictly. This does not replace
an actual downstream consumer build.

The spec/corpus, x86 JIT matrix, installed consumers, and actual downstream
compile gate require separate recorded runs. Passing the SDK execution target
or selected contract probes does not establish those gates.

## Historical Allocator Plan: Read-Only Assessment

At that checkpoint the WAMR bridge checked the caller's `wasm_allocator` but did not use its
mapping. This is a real ownership/API blocker, not a supported alternative.

Upstream `core/iwasm/include/wasm_export.h` defines `MemAllocOption`,
`Alloc_For_Runtime`, and `Alloc_For_LinearMemory`. With
`WASM_MEM_ALLOC_WITH_USAGE`, `core/iwasm/common/wasm_memory.c` sends initial
linear-memory allocation, growth, and teardown through the usage callbacks.
The callbacks and optional user data have process-runtime lifetime; replacing
them for each execution would race and is not acceptable.

Proposed integration, not implemented in this step:

- Register one stable dispatcher when the single WAMR runtime is initialized.
  Keep runtime allocations on the system allocator.
- Scope a thread-local owner around every instantiate, guest call, and teardown,
  including constructor-failure cleanup. Nested scopes restore the prior owner;
  parallel threads have independent owners. No guest worker threads, shared
  memory, or multi-memory are admitted by this profile.
- Borrow one existing `wasm_allocator` exclusively for the full instance
  lifetime. Its existing mapping/page operations remain the sole linear-memory
  manager. Reentry into that instance reuses its owner; a second live instance
  using the same allocator must be rejected, not reset under the first one.
- Verify callback pointers against the active owner's mapping. Growth commits
  pages in place; no copy to a second linear-memory buffer. Teardown releases
  the instance's borrow, not the caller-owned mapping's lifetime.
- Capture callback exceptions without unwinding across C. Surface local
  allocation/protection failures as operational failures, not guest traps.
- Enable the upstream usage flag only through the coordinated common runtime
  profile (`WAMR_BUILD_ALLOC_WITH_USAGE=1`), with matching identity/build
  definitions. No public WAMR types or vendor header installation is needed.

Before implementation is accepted, prove zero initial pages and first grow,
fixed base address, retained data/zeroed new pages, module/caller grow limits,
fresh reset, nested and parallel ownership, shared-allocator rejection, teardown
after partial initialization, and host/cancellation/physical-resource failures.
Callback scopes alone do not enforce ownership; the allocator borrow and all
failure paths need concrete tests. No allocator implementation is changed by
this assessment.
