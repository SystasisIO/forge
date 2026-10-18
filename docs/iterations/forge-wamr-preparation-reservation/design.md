# WAMR preparation reservation: stage-1 prototype and approved continuation

Status: experimental private allocation-owner prototype, not loader integration.
`artifact::prepared_byte_size()` remains fail-closed. No fork, runtime allocator,
prepared-module/context, session, compiler or executor implementation is changed
by this stage. The prototype does not establish an exact native artifact plan.

## Approved continuation

On 2026-10-16 the maintainer approved using a conservative, enforced preparation
ceiling instead of requiring an exact prospective size. The historical stage-1
description below remains evidence for the isolated prototype. Exact ledger
replay is no longer mandatory if the real loader can enforce a proven ceiling
before each allocation and mapping without another WASM/AOT parser.

The implementation must precharge retained storage and temporary overlap,
including C++ bridge allocations, through the existing session accounts. Actual
retained storage must not exceed the reserved ceiling; unused reservation is
returned only after the matching storage is gone. Cancellation, OOM and partial
loader failures must unload unpublished state before releasing its charge.
Requested storage accounting is not a process RSS guarantee. A guessed input
multiplier or measuring an already unbounded allocation does not satisfy this
decision. Direct preparation requires the same enforceable ownership boundary.

The public `prepared_byte_size()` contract change requires an explicit release
note and updated callers/tests. Until real loader/bridge/session integration is
qualified, the current fail-closed implementation remains the correct gate.

## Current implementation and ownership

The private pair is
[`details/engine_load_plan.hxx`](../../../libraries/vm/wasm/interpret/details/engine_load_plan.hxx)
and [`engine_load_plan.cpp`](../../../libraries/vm/wasm/interpret/engine_load_plan.cpp).
It is compiled only by the
[isolated probe](../../../tests/vm_wasm_optimizing/preparation_plan/CMakeLists.txt),
not by `forge_vm_wasm_interpret`. The parent registered the isolated directory
in the existing optimizing test graph after independent review. That registration
does not wire the owner into production.

The component has no WAMR includes or parser, does not initialize a runtime,
does not create an instance, and contains no executable entry/publication path.
One caller exclusively owns each instance. Concurrent or reentrant use of one
instance is unsupported; independent owners share no mutable global state.

## Explicit bounded workspace

Every construction supplies all four limits: workspace bytes, event slots,
simultaneously live block slots, and intended native page size. There is no
production workspace constant or implicit unlimited mode. Probe budgets are
test inputs, not proposed product defaults.

`base_workspace_bytes(events, live_blocks)` includes `sizeof(engine_load_plan)`,
the exact event array, the exact live-block array, and their alignment padding.
The initial budget check happens before the single metadata allocation.
These fixed arrays cannot grow; there is no hidden vector/map allocation.
The caller owns the stop state and allocator context; their external storage is
not silently claimed as planner-owned storage.

Each payload allocation reserves its requested backing bytes against the same
workspace limit before calling the injected allocator. Runtime zero-size malloc
is represented as one byte, matching the WAMR runtime malloc normalization.
The prototype also normalizes a zero-size resize to one byte; production resize
ABI compatibility must be reviewed before adapting the real callbacks.

`reallocate` allocates/copies/releases, so old plus new backing bytes are charged
simultaneously. A failed resize leaves the old block owned and unchanged.
After replacement allocation, a cancellation check precedes the commit point.
Cancellation there releases the uncommitted replacement and its backing charge,
without changing the old pointer, bytes or committed ledger. Passing that check
is the resize commit point: copy/release/ledger update completes without another
failure check, and success returns the new pointer even if cancellation arrives
while releasing the old allocation. The next allocation observes pending stop.
`peak_workspace_bytes` reports peak *reserved* workspace; an injected allocation
failure can therefore contribute a reservation that was not physically allocated.
Allocator-private metadata and process RSS are outside this requested-storage
contract. The allocator must return max-align-aligned storage and must not throw
or reenter the owner.

## Shadow maps and ledger

`map(bytes, executable)` creates only a malloc-backed shadow. The caller supplies
the page size: no platform query or loader mapping occurs. Native retained size
is `round_up(bytes, page_bytes)`. Temporary shadow backing additionally includes
`page_bytes - 1` alignment slack. Both calculations are overflow checked before
allocation. Shadow pages are zero filled and page aligned.

The executable flag is ledger intent only. No protection changes or code
execution occur. The archive's undefined-symbol check rejects direct references
to native mapping, protection, JIT write-protection and cache-flush functions.
This is not a claim that the system malloc implementation never internally uses
OS mappings; it is a check that this owner has no native-loader effects.

Every successful alloc/map/resize records a stable block ID, operation kind,
requested bytes, retained bytes, backing workspace, previous resize bytes,
alignment and executable intent. Runtime bytes and prospective native mapping
bytes are separate observations. These observations describe this scripted
prototype's live blocks, not WAMR structural module-memory measurements.

An allocation needs room for its own event AND its eventual cleanup event.
Each live block reserves that cleanup slot. Resizing consumes one additional
event without consuming the cleanup reservation. Thus ledger exhaustion cannot
force unjournaled payload cleanup. Release/unmap validate pointer ownership and
allocation kind before calling the allocator. The live-block table is a bounded
linear scan on the cold planning path, not a cache or scheduler.

Budget, journal, overflow, allocator and cancellation failures latch a typed
reason. Subsequent allocations fail without invoking an allocator fallback.
Release/unmap and allocation-free `rollback()` remain valid after failure.
An ordinary allocation/map that observes cancellation immediately after malloc stays owned
even if its API returns null: rollback/destruction still releases it. This is
intentional so failure never leaves a hidden allocation. A WAMR adapter must
unload its unpublished module before destroying its raw storage owner.
Resize instead drops an uncommitted replacement on cancellation as described
above; it must never return null after freeing the caller's old pointer.

Rollback emits the reserved cleanup events and is idempotent. Destruction
performs rollback and then releases the metadata slab. No runtime hooks, input
identity, sealing, commit replay or publication have been implemented.

## C++ storage boundary requiring separate review

Read-only inspection of the current bridge identifies these preparation costs:

| Owner | Retained storage | Temporary/duplicate storage and required treatment |
| --- | --- | --- |
| `artifact::impl` | Existing payload, encoded bytes, metadata charge | Keep the existing decoded-artifact charge; do not count payload copies as zero-cost borrowed inputs. |
| `native_image` | Object, module shared owner, native-function vector | Initial `vector<host_binding>`, copied names/signatures, frame-size vector, and each `get_function_type()` parameter vector need precharged workspace or allocation-free inspection. |
| `prepared_module` | Object, impl and public `imports` vector; each import owns module/field byte vectors | `project_imports()` currently allocates after impl construction. For a retained-prebuilt approach, construct and bound these objects in plan and move the SAME storage into the final owner. Rebuilding during commit is not an exact plan. |
| `prepared_module::impl` | Source/native shared engine owners, shared bindings, unique limits owner | Source/native input vectors, control blocks and any construction intermediates must be counted explicitly. |
| Private `engine_bindings` | Vector of functions, module/name strings, parameter vectors, callable state | `binding(host_binding)` moves names into its function record but also captures the remaining host binding, including signature vectors and its callback, in another `std::function`. Parameters therefore have duplicate representations and opaque callable allocation. |

The artifact adapter's `[i]` dispatch lambda has a fixed known capture. That does
not make the larger private captured `host_binding` free or establish portable
`std::function` small-buffer allocation behavior. Likewise, measuring default
`std::vector`/string capacities after an unguarded construction is not a bounded
allocation mechanism.

Preferred follow-up: retain one immutable, explicitly bounded preparation/binding
owner, reuse its existing bindings in both source and AOT slots, and replace only
the private captured-host-binding duplication with indexed dispatch against that
same owned state. Do not introduce a second public callable wrapper/registry.
The existing `engine_bindings` lookup/attachment/dispatch remains the owner.
Default-allocator public `imports`, private shared-owner control blocks and opaque
callable storage still need an enforceable allocation strategy before that path
can be called bounded. This stage neither changes them nor claims to solve them.

## Proposed fork hooks, still unapproved and unimplemented

Use the existing WASM/AOT loader, not an independent format parser. A future
scoped load owner must route generic `Alloc_For_Runtime` allocations and native
section effects, without interfering with the usage-1 linear-memory owner.
Pointer ownership must survive unload on a different exclusive caller thread;
TLS alone cannot decide which allocator frees a committed module's storage.

The AOT hook inventory remains `loader_mmap`, `destroy_sections`,
`try_merge_data_and_text`, `load_relocation_section`, `aot_load_from_aot_file`
and `aot_unload`, including protections, JIT write-protection and cache flushes.
The malloc-only pass records the chosen layout and allocation lifetimes.
Commit reuses the same loader and enforces the planned ledger. Address-dependent
relocation application remains commit work, not something certified using
unrelated malloc addresses. A planned merged layout cannot silently fall back
to another layout on commit failure. Temporary mapping overlap and the Linux
platform mapper's temporary huge-page alignment allocation also need accounting;
page-rounded final size alone is not peak workspace proof.

Parent reserves `WASMBlockTypeMode`, `LoadArgs.block_type_mode`,
`WASMModule.forge_block_type_mode` and their loader/compiler/Forge propagation.
The future plan identity must bind this policy along with inputs, exact engine
profile and page size. No new LoadArgs field or shared fork branch change is
authorized by the stage-1 prototype.

## Session and direct API gates

The existing worker order remains:

1. Obtain a bounded plan, with temporary storage charged to existing inflight accounting.
2. `reserve_resident(exact_prepared_bytes)`.
3. Commit with only planned retained allocations and precharged temporary overlap.
4. Require actual owner ledger and `byte_size()` to equal the plan.
5. Publish ready completion/cache only after success and cancellation checks.

Future session integration must charge its existing inflight account before
each allocation, including ledger storage and resize/mapping overlap, then drop
storage before releasing its charge. Existing decode workspace multipliers are
not a proof of the new planner's workspace and must not be reused as one.
The prototype currently enforces only the supplied local workspace ceiling;
it is not connected to session accounting. The compatible explicit overload or
reviewed finite policy for direct `prepared_byte_size()` calls is still open.
There is no silent unbudgeted direct path.

## Verification and limitations

The probe covers metadata rejection before allocation, exact-byte acceptance,
one-byte excess, reserved cleanup at journal exhaustion, live-block limits,
4 KiB/16 KiB page rounding/alignment, resize overlap/success/OOM, cancellation of
a 48-to-96-byte replacement before commit and cancellation after the old block
is released, failure at
every allocation in a scripted load-shaped sequence, cancellation before and
during allocation, pointer-kind/owner isolation, checked overflow and idempotent
rollback/scope-exit cleanup. A symbol-level test checks absence of direct native
effects. Sanitizers can be enabled with the probe-only CMake option.

Standalone commands, from the repository root on the current macOS toolchain:

```sh
cmake -S tests/vm_wasm_optimizing/preparation_plan -B build/wamr-preparation-plan-stage1 -G Ninja \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/wamr-preparation-plan-stage1 -j2
ctest --test-dir build/wamr-preparation-plan-stage1 --output-on-failure -j2
```

Verified locally on 2026-10-16 with Homebrew LLVM 22.1.8, Ninja, and `-j2`:
The Curie P2 regression failed against the prior resize implementation with
`Cancelled resize lost the original pointer or its 48-byte ownership` before
the fix. After the commit-point correction, 16/16 ordinary Debug tests passed
in `build/wamr-preparation-plan-stage1`; 16/16
AddressSanitizer/UndefinedBehaviorSanitizer tests passed in
`build/wamr-preparation-plan-stage1-asan`. The latter configures the same source
with `FORGE_WAMR_PREPARATION_PLAN_SANITIZERS=ON`. Both runs include the direct
native-effect symbol check. The parent subsequently registered this isolated
probe in the existing main test graph after review; its root Release run passed
16/16. Production activation was not performed by stage 1.

These are not real-loader, exact C++ binding/storage, commit-replay, Linux native
execution, cache-publication or consumer acceptance tests. Passing them does not
authorize replacing the fail-closed production reservation gate.
