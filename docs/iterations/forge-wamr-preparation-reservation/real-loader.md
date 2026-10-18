# Bounded real-loader owner: review checkpoint

Status: private real WASM/AOT loader qualification, pending combined fork review
and production pin integration. This is not full runtime-migration acceptance.
The historical malloc-only stage-1 prototype remains separate and unchanged.

## One storage owner

The private pair is
[`engine_load_budget.hxx`](../../../libraries/vm/wasm/interpret/details/engine_load_budget.hxx)
and [`engine_load_budget.cpp`](../../../libraries/vm/wasm/interpret/engine_load_budget.cpp).
Every bounded creation supplies a caller ceiling; the implementation has no
production default, input multiplier, second parser, cache or scheduler.
`prepared_module::preparation` and private C++ bridge storage use this same
owner, not an additional reservation counter.

`create()` checks the ceiling and cancellation before requesting exactly
`sizeof(engine_load_budget)` bytes. The root physically contains a 256-byte,
max-aligned control-block slab. The custom standard allocator checks the rebound
type's requested count, size and alignment before returning that storage; it
cannot allocate elsewhere. Insufficient slab capacity is a typed refusal, not
an assumed STL layout. `root_byte_size()` is exactly `sizeof(owner)`, including
the slab, mutex and counters. No private STL types or libc++-only root branch
remain. Local validation uses libc++ on AArch64; other STLs are not yet tested.

The shared-pointer deleter intentionally leaves the physical root intact.
Final control-block allocator deallocation, including final weak references,
destroys and frees the root, then drops the independent admission-charge token.
Failed bootstrap retains a stack guard until the control block is adopted.
`pin_charge()` is an exclusive, one-time, prepublication operation. The supplied
token must not own the preparation handle, root or a budget-allocated prefix:
that would introduce a cycle. Session integration must attach it before loading,
not only after successful sealing.

Requested storage means caller-requested buffers, owned headers, alignment slack
and page-rounded native mappings. It is not RSS, allocator-private bookkeeping,
thread stacks or C++ ABI exception-runtime storage. Global WAMR initialization,
caller input, caller callbacks' own heaps and subsequent caller mutation of
public containers are outside module-owned storage. The separate public bridge
must qualify its initial C++ projection; this private harness does not establish
that public-container allocation contract.

## Heap, mappings and rollback

`engine_memory` routes only generic runtime allocations through the owner.
Linear-memory instance allocation retains its existing distinct owner.
Each generic block has a charged prefix containing the original shared owner,
raw backing pointer, bytes and alignment. Unowned global runtime blocks use the
same prefix layout with an empty owner. Free and realloc therefore work on
another thread without the preparation TLS scope.

Allocation precharges the complete backing request. Cancellation after backing
malloc frees the provisional block and prefix. Realloc precharges old plus new,
then checks cancellation before copying or releasing the old block. That check
is its commit point: no later failure can return null after the old pointer was
freed. A refused resize preserves the old pointer, data and charge.

Each native mapping first allocates its own charged ownership record, then
reserves the native request before calling the existing WAMR mapper. Mapping
failure releases both requests. Successful unmap releases bytes only after the
checked OS unmap succeeds. Failure retains the address, record, physical owner
and charge token; allocation-free retry runs after loader cleanup. Persistent
OS refusal deliberately remains charged, even if the caller drops its handles.
There is no refund of still-mapped storage. Failed constructor/unload retry
transfers reachable teardown ownership to the existing `engine_runtime`.

Each admitted root physically contains exactly one intrusive runtime membership
and its ownership pin, already included in `root_byte_size()` before the first
mapping. Simultaneous failures cannot exhaust membership storage or allocate
failure-path metadata. Registry entry count is bounded by the still-charged
admitted root storage, not an additional guessed process-wide root count. The
original session token continues to enforce its existing resident/inflight cap;
independent callers do not receive a new global process-RSS guarantee.

Explicit `retry_pending_cleanup(max_owners)` attempts at most that many existing
roots and reports attempted/remaining. Before first runtime initialization it
returns zero without allocating or initializing WAMR. No background worker,
cache, scheduler or failure-path allocation is added. Root-list locks never
cover native syscalls or physical frees. A successful per-owner retry removes
its runtime membership; concurrent new failure registration cannot disappear
behind an in-progress drain.

The artifact/session integration must additionally retain the failed preparation
and artifact in its existing job lifecycle, retry before declaring shutdown
complete, and retain its charge on repeated refusal. The instance preparation
retry reports only its own failed mappings; the static remaining count is
process-global advisory information, not proof that an unrelated session owns
pending work. Runtime retention also covers successful modules whose final
caller handle is destroyed during a persistent native-unmap failure.

Bounded Linux mapping opts out of optional huge-page alignment overmapping with
`MMAP_MAP_NO_HUGE`. This removes hidden unchecked prefix/suffix trim operations;
there is one owned page-rounded extent. Ordinary upstream calls retain their
existing huge-page behavior. Text/data merge still precharges old and new
extents simultaneously. PLT, GOT and loader temporary allocations follow the
same hooks, not retrospective 32-bit WAMR memory metrics.

Linux AOT uses ordinary RW staging and a checked final RX transition after
relocation (`MMAP_MAP_NO_JIT`). Intermediate merge never needs execution
permission. Every data page, including default sections and padding, is
explicitly narrowed to RW/non-executable before named read-only sections become
R/non-executable. There is no unconditional cross-platform RWX workaround.

Darwin AArch64 preserves the existing native MAP_JIT text mechanism: text has
RWX VMA permissions, with effective W^X enforced by the supported pthread
per-thread mechanism. VMA RWX alone is not presented as proof of effective W^X.
Data uses ordinary non-JIT mappings with checked RW/R permissions. Optional
text/data merge is disabled there because mixed MAP_JIT data cannot be narrowed
independently. Final text protection uses the existing pthread wrapper rather
than ignoring a failed mprotect on MAP_JIT. The platform contract follows
[Apple's JIT guidance](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon).
Hardened-runtime entitlement combinations remain unqualified. Callback-allowlist
entitlements that prohibit the existing pthread setter are not newly supported.

The AOT entry saves and restores the prior WAMR-wrapper-managed thread JIT state
on success and failure. The OS SDK provides no current-state getter here;
external direct calls bypassing the WAMR wrapper are not observable. Nested
wrapper-managed states, both enabled and disabled, are regression tested.

`seal(resident_limit)` validates retained storage and reduces the effective
allocation cap to actual retained bytes. Refunded reservation cannot later be
reused as uncharged headroom. Module unload retains the owner and an allocation
scope through every loader cleanup call.

## Fork And Build Interface

Fork file set submitted for review by this checkpoint:

- `core/iwasm/include/wasm_export.h`
- `core/iwasm/include/wasm_c_api.h`
- `core/iwasm/aot/aot_runtime.h`
- `core/iwasm/aot/aot_loader.c`
- `core/shared/platform/include/platform_api_vmcore.h`
- `core/shared/platform/common/posix/posix_memmap.c`
- `core/shared/platform/common/posix/posix_thread.c`

`WASMLoadBudget` is an optional per-load callback record mirrored in both
LoadArgs declarations: begin/complete mapping, checked unmap, cancellation check,
before-protect fault gate, and typed allocation/protection failure notification.
Its context must outlive unload. AOTModule stores the copied record. Forge uses
a pointer to its immovable module's shared-owner member as context.

Parent production CMake needs `interpret/engine_load_budget.cpp` in the existing
interpret target. No new production library is needed. The optional executable
source is `tests/vm_wasm_execution/load_budget_probe.cpp`; it needs the existing
engine-memory test's private includes/definitions and two explicit fixture paths.
Parent owns combined source publication and the production source pin.

The adjacent WASM C API binary-only cache must refuse per-load budget contexts,
as it already refuses nonstandard block policy. This cache-policy hunk is owned
by the parent and was not changed in this checkpoint. Forge uses load_ex rather
than that C API cache. Production integration must not overlook this guard.

## Qualification

The isolated
[harness](../../../tests/vm_wasm_execution/load_budget/CMakeLists.txt)
requires an explicit fork checkout and existing WASM/AOT files. It never edits
or bypasses the production source-pin mechanism. Small graphs live under the
existing stage-1 Debug and ASan build directories; all builds use `-j2`.

Local Debug and ASan/UBSan runs pass real source+AOT load/unload, final weak-pin
root storage, insufficient root/slab refusal, seal headroom, aligned storage,
cross-thread free/unload, provisional-allocation cancellation, 48-to-96 realloc
preservation, and persistent-unmap owner/token retention followed by retry.
The updated harness queries actual data/default/padding page permissions with
Mach on Darwin or procfs on Linux and exercises a native data mapping larger
than 2 MiB. It tests a real module's repeated unload refusal after dropping all
caller references, then recovers through the existing runtime maintenance API.
The Linux execution lane is not available on this host and remains NOT_RUN.
Five real-loader fault sweeps cover 43 allocation, 2 mapping, 2 protection,
43 cancellation and 2 unmap refusals with complete rollback. Debug and ASan/UBSan
each pass fifteen repeated runs of this checkpoint. The fixture peak
is 37,543 requested bytes;
that observation is test evidence, never a production prospective estimate.

Remaining acceptance: independent review, combined fork pin, public C++ bridge
and session accounting integration, additional platforms/STLs and broader native
fixtures, actual execution/profile regression lanes, and the separately owned
LLVM JIT native-map qualification. A total LLVM transient IR/compiler-heap cap
was not the previous backend guarantee and is not an added closure gate. This
owner does not claim to account that heap. No production activation or complete
migration claim follows from this checkpoint alone.
