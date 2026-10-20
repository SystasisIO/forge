# WASM Optimizing

Status: Experimental WAMR port. Not production-ready.

Authoritative current status:
[2026-10-18 R5 integration report](../../../../docs/donors/forge-wamr-dev-integration-20261018.md).
The current source pin is `12d1f906668cde93c03e7d864e71ee1e10dfb194`.
On Darwin ARM64/LLVM 22.1.8, the fresh source AOT/preparation/session/cache/cleanup
subset passed **47/47 CTests** in
[runtime-matrix-r2.xml](../../../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-matrix-r2.xml).
The installed V4 compiler/export-name/runtime/helper-session fixture passed
**5/5 nested CTests**:
[outer package runner](../../../../../../artifacts/wamr-dev-integration-20261018-r5/package-optimizing-r4.xml),
[nested results](../../../../build/wamr-dev-r4/forge-package-vm-wasm-optimizing-component/package-tests.xml).
The outer runner's 1/1 is orchestration, not an additional sixth test.

These are bounded development qualification results, not full historical parity,
proof of every admitted source shape, or production/product acceptance.
x86 execution, LLVM-free installed runtime and full product integration remain
NOT_RUN. Earlier `156dbd0`, `75e8f91` and older checkpoints retain their original
identities/results. Rebuild the runtime, actual compiler helper and fixtures
together after an identity change; never relabel old output.

The optional `forge_vm_wasm_optimizing` target retains the current owning
`compile_request`, `compile_configuration`, `target_profile`,
`compiler_session` and executor contracts. Public leaf modules are
`compile_request`, `artifact`, `invocation_abi`, `compiler_protocol`,
`compiler_session`, `executor` and `exceptions` under
`forge.vm.wasm.optimizing`.

The API source baseline is the current qualified owning snapshot
`snapshot-policy-metadata-owning-v2.46UxhQ`, with the later existing
SIGCHLD/session ownership fixes from its donor source. It is not the older
frozen b40 artifact API. Donor provenance remains in the
[WAMR baseline](../../../../docs/donors/forge-wamr-baseline-v1.json).

## Ownership

The existing session owns bounded admission, canonical identity verification,
deduplication, helper jobs, child reap, cache ownership and shutdown. It uses
the existing injected Forge Asio runtime/compute executor and bounded scheduler.
No second queue, parser, cache, timer worker or execution engine is introduced.

The common `forge_vm_wasm_interpret` owner supplies original-source admission,
immutable module-local host bindings, source-plus-native `prepared_module`,
`runtime_context`, memory checks, floating-point state and cancellation.
No SDK execution module or WAMR public C type enters this leaf.
Default interpreter/AOT runtime consumers remain LLVM-free. LLVM belongs to
the optional [compiler](../compiler/README.md) and isolated helper, or to the
separately enabled `FORGE_ENABLE_VM_WASM_LLVM_JIT` runtime option (default OFF).
The AOT helper always has JIT disabled. Optional native WAMR LLVM JIT has real
Darwin ARM64 compile/bridge/maps and public profiling execution evidence in the
[later R5 public suite](../../../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-matrix-r4.xml).
That suite passes the nine-case public profiling CTest after its reviewed
test-only cancellation correction; the earlier R2 failure remains historical.
This is upstream WAMR native codegen, not a custom backend or the retired donor
JIT. x86 execution and LLVM-free installed-runtime qualification remain NOT_RUN.

Artifact codec/identity implementation units do not reference runtime preparation.
The helper can therefore link only the compiler's WAMR C implementation.
Native preparation and execution use the common runtime's implementation in
the parent process. Same-process compiler/runtime coexistence is NOT supported
or qualified by the separate-helper link-map evidence.

## Native Artifact Trust

The WAMR envelope is artifact version 2, codegen version 5 and private protocol
version 3. It retains original WASM and AOT bytes plus checked source metadata.
Identity binds source digest, canonical execution options/imports, exact
engine/profile/capacity identity and closed native target/version fields.
Old OC identities and foreign engine profiles require rebuilding original WASM.

Hashes in this mutable envelope do not authenticate that native instructions
implement that source. Shape/type/export checks are defense in depth, not proof
of correspondence. Arbitrary untrusted AOT must never execute.

`artifact::decode` and `from_payload` default to `origin::untrusted`.
Executable authority is an explicit host assertion stored OUTSIDE serialization:
the existing verified private helper channel supplies `isolated_compiler`;
the existing private, exclusively owned cache supplies `owned_cache`.
Ordinary decode does not regain authority, even after recomputing the checksum.
Preparation checks this authority before calling the common AOT constructor,
which independently requires an explicit trusted-origin argument. Native
application code deliberately asserting trust is part of the trusted host,
not a sandbox escape addressed by an envelope checksum.

The cache trust assumption remains a private 0700 owner-controlled directory,
checked descriptors/inodes/identity/configuration/checksum and one lifetime
exclusive lock. All cooperating writers obey that lock. Noncooperating same-UID
edits while owned are unsupported; this is not cryptographic cache authentication.
Never relabel old artifacts or compiler binaries with a new source identity.

## Execution And Resources

The existing cold native-stack/thread owner remains. Guest entry uses its mapped
stack; host callbacks bounce onto the ordinary host stack. Native frame sizes
come from the common WAMR loader's generated stack metadata. Logical source
storage/depth admission is distinct from physical stack availability.
A physical shortfall is operational `execution_unavailable`, never a different
canonical guest verdict selected by a host's stack size.

Deadline/cancellation use the existing executor epoch scheduler and a private
stop token consumed by the common runtime. Typed host exceptions are retained
outside generated frames and take precedence over racing cancellation.
Initialization/start is inside the invocation's cancellation and stack boundary.
Code mappings are never protected/unmapped as a cancellation mechanism.

Direct `prepare()` is explicit cold work. Borrowed `native_code()` and
`functions()` views remain pinned by artifact copies. `runtime_module()`
returns an aliasing pin that retains the entire artifact owner, including its
decoded storage and resident charge, rather than only the inner common module.
Keeping that pin prevents sole-owner eviction even after other artifact copies
are dropped. It never performs implicit preparation.
No ready lookup or warm invocation decodes, loads or compiles lazily.

## Raw Entry Migration

The explicitly approved source-incompatible change removes `artifact::entry()`,
`entry_function` and `native_function::entry`. WAMR code does not implement the
old uniform OC machine calling convention. The unused raw callback table/types,
table entry record and 168-byte `invocation_abi` machine context are also removed;
no throwing placeholder, compatibility cast or synthetic trampoline remains.
The existing module name `invocation_abi` still owns the real `trap_reason` and
`native_function` inspection contracts. Host binding identity is version 3;
codegen remains version 5. Rebuild earlier artifacts from original WASM.

Consumers inspect type/frame metadata through `artifact::functions()` and call
the existing executor binding/invocation API, or the high-level compiled module.
`native_code()` is a borrowed inspection view, not an executable entry contract.
The shared host API, compiled module, executor and session/cache remain owned by
their existing components. The existing epoch scheduler remains; only its
unread legacy machine-context writes were retired.

## Preparation Reservation

The accepted contract is a conservative, enforced reservation ceiling, not a
promise of the exact future `byte_size()`. Reserve the retained upper bound
before preparation and separately admit its temporary/peak workspace against
the existing inflight account. Every loader allocation, resize overlap, native
mapping and C++ bridge allocation must fit these explicit budgets before it is
performed. Successful preparation must report `actual <= upper`; temporary
storage must be released before its charge or the unused reservation is returned.
The retained charge must follow every owning artifact and runtime-module pin,
including after session shutdown, and is released only after the last physical
storage owner has been destroyed.

`artifact::prepare(limits, stop, charge)` uses the common
`prepared_module::preparation` allocation owner for WAMR heap/maps, adapter
tables, bindings and C++ control blocks. `workspace_bytes` bounds total live
preparation storage, including retained storage under construction and temporary
overlap. `resident_bytes` bounds its final retained storage. `seal()` refuses an
oversized result and freezes future growth at actual before any refund.
`prepared_byte_size(limits)` reserves existing artifact storage plus the resident
ceiling; it neither loads nor predicts loader allocation sizes. A locally admitted
artifact can still fail this resource policy. Structural metrics and guessed
size multipliers are not allocation bounds.

The source-compatible no-argument overloads use the explicitly named
`artifact::standalone_preparation_policy`: 64 MiB resident and 256 MiB peak
workspace. These are standalone local resource caps, not size estimates,
function-stack limits, or a guarantee of fit. The session never uses this policy.
It uses caller `max_preparation_resident_bytes`/`max_preparation_workspace_bytes`.
Zero preserves source initialization by deriving per-load policy ceilings from
the existing finite session budgets, then intersecting them with the current
unreserved account headroom atomically before charging:

- Resident: `(max_resident_bytes - fixed metadata) / worker_count`, less the
  artifact's owned bytes, indexed configuration and charge-token allocation.
  First try the existing ready-entry eviction. If external/weak pins still
  retain charge, reduce only the inherited native ceiling to the remaining
  headroom less the decoded artifact's owned bytes. Refuse unless those owned
  bytes and a positive native ceiling fit; no predicted loader minimum is used.
- Workspace: `(max_inflight_bytes - fixed metadata) / worker_count`, less the
  job's already-reserved decode/request storage and the bounded ticket allowance.
  Also intersect with current unreserved inflight headroom, including all
  already-admitted jobs/tickets and other active preparations.

These are explicit admission quotas, not promises about accepted WASM size.
`max_tickets` bounds retained outcomes/index capacity, not compiler concurrency;
raising it changes actual metadata costs but does not divide the per-load
resident ceiling. Both default shares use active compiler worker concurrency.
Pending jobs and external pins still consume the aggregate quotas; the resulting
default ceiling is an enforced admission cap, not a fit guarantee. Insufficient
capacity refuses work through the existing typed path, without a new queue.
Explicit nonzero per-load ceilings replace the corresponding default share and
are never shrunk to headroom; insufficient aggregate capacity refuses them.
Preparation reservation holds the account mutex only for arithmetic, never
across eviction, destruction or loading. Weak-pin charge is not refunded by
ceiling reduction.

The session reserves resident and peak quotas before constructing the allocation
owner. Successful sealing and temporary destruction precede returning unused
resident/peak reservation. A failed native cleanup keeps both reservations with
its physical owner. The artifact retains the neutral preparation handle before
native loading starts. A failed session load moves that artifact into the SAME
live job's cleanup slot. `artifact::retry_cleanup()` releases this handle only
after physical cleanup succeeds. Persistent refusal delivers `shutdown_incomplete`,
stops admission and keeps the live job reachable; explicit `shutdown()` retries
and refuses quiescence while that job still owns failed cleanup. Charge-token
metadata is itself precharged before allocation.
External tokens must not own their artifact/preparation or use that preparation's
allocator. Artifact control-block retention and the common owner both pin the
token; even weak module pins retain charge until physical control-block release.
Shutdown drops ready-index ownership, not external pins. `session.usage()` reports
the shared resident/inflight account while the session object remains alive.
The common runtime's prepaid intrusive retention also covers failed unmap after
the final successful artifact/module pin is dropped. Cold session finish/shutdown
call bounded `preparation::retry_pending_cleanup(max_owners)` maintenance; direct
owners may call the same allocation-free facade after all pins are gone. Its
process-wide remaining count is advisory and never blocks an unrelated session's
shutdown. Instance-owned failed jobs remain the session's authoritative drain gate.

Bounded initial default-allocator string/signature projections require the
reviewed libc++ 22.1.8 allocation contract and reserve before copying. Other
versions/standard libraries fail closed. Fixed index callback captures are
statically checked for libc++ inline storage. Caller callback-owned heap and later caller mutation of
the public module `imports` vector are outside Forge's preparation accounting.
Requested storage and owned headers are counted, not allocator internals or RSS.
Quota, allocation and protection refusal map to optimizing `compile_limit`;
preparation cancellation maps to `cancelled`. Repeated calls check stop and the
new resident ceiling and never attach/replace a different resident token.

### Historical Pre-R5 Qualification Note

The following pending-gate paragraph records the earlier source checkpoint;
its status is superseded by the bounded R5 results at the top of this README,
not by rewriting the historical evidence:

This integration is source-pending: fresh combined-fork real-loader/session
tests and independent review are still required. It is not a green migration claim.
The reservation issue is not covered by the accepted retirement of huge historical
capacities or the separate raw-entry API removal. External allocator observable-state parity, complete typed trap
mapping, shared feature-profile identity, Linux execution, installed consumers
and product integration also remain acceptance gates. The supported common
profile rejects the 65,536-local fixture without widening WAMR counters.

## Session Lifecycle And Checks

Constructing a session performs no helper/filesystem work. Await explicit
`initialize(target_profile::native(), stop)` before admission; it owns the real
HELLO/ACK/EOF exchange, sole physical child reap and bounded cache preflight.
Failure keeps admission closed. `enqueue` only admits bounded unverified work;
worker-side canonicalization verifies the hint before cache/helper mutation.

A ticket cancellation affects its consumer, not shared compilation. `ready`
is immediate and requires full canonical configuration equality. The same
cache catalog charges selected, quarantined, opaque and temporary payloads.
Unknown files are preserved; replacement reserves old plus new bytes.

Unmanaged Boost.Process v2 async wait is the sole child reaper. The embedding
application must establish default SIGCHLD, no SA_NOCLDWAIT/competing reaper and
keep runtime/compute pool, executor and storage alive through cleanup. On Darwin,
the first cold session uses the shared `forge.asio.child_signal_registration`
guard and requires exact native default (`SIG_DFL`, zero flags, empty mask);
custom flags/masks on a default handler are rejected without mutation. The guard
uses the existing public Asio signal set. Other sessions, including sessions on
distinct runtimes, may join only while known own registrations are live and the
captured handler, flags and mask are unchanged. Each session retains its own
runtime-bound registration until its child/task/compute drains. Checked public
unregister failure or an observed foreign disposition change returns
`shutdown_incomplete`, preserving the SAME registration and runtime; quiescence
is not claimed. The last successful Asio unregister restores default disposition.
Failed startup after registration also requires explicit shutdown before
destroying injected owners.

VM boundary exceptions remain `compiler_unavailable` for acquisition failure and
`shutdown_incomplete` for release failure. The stricter cold profile changes
which host dispositions are admitted; public error categories stay unchanged.

This is ownership coordination, not a signal handler or reaper replacement.
Child waits remain exact-PID Boost.Process operations. The shared guard's private
mutex serializes only cooperating Forge registrations; it cannot synchronize
unrelated signal mutation or foreign waitpid calls. An embedding host must not
replace the handler or introduce a competing reaper while registrations are live.
Checks reject observed changes at acquire/release, not continuously monitor the
process or prove the absence of unsynchronized external changes. If the first
post-registration disposition cannot be captured, no later owner is admitted
and cleanup retains unconfirmed registration rather than guessing ownership.
`compute::pool::request_stop()` MAY precede `session.shutdown()`: cleanup
submits no new compute work, but waits for already submitted verification/
spawn/cache operations to publish ownership or unwind. The Asio runtime must
continue servicing child reap/timers and task drain. Join/destroy injected
owners only after successful session shutdown. Shutdown closes admission, cancels
retained live handles outside the index lock, escalates only owned children and
drains their callbacks/reap plus scheduled work. The deadline is SIGKILL
escalation, not a promise about physical OS reap latency. Typed
`shutdown_incomplete` retains state for explicit retry; destroying a
non-quiescent owner violates lifecycle and terminates rather than blocking or
detaching cleanup. Successful terminal delivery never allocates a new outcome.

The [focused tests](../../../../tests/vm_wasm_optimizing/README.md) separate
actual live native frames, cancellation/reuse, untrusted substitution and
bounded preparation/session/cache behavior from historical fail-closed checkpoints.
