# Darwin ARM64 recovery contract — source-design gate

Status: existing-owner implementation in progress; combined source/runtime acceptance HOLD.
Human scope now requires both Linux x86-64 and macOS ARM64 native OC. The earlier
POSIX-only altstack diagnosis remains immutable evidence, not an unsupported
Darwin product decision. Wasmtime is a platform donor, not a dependency, second
VM, parser, executor, stack framework or artifact cache. Existing v3 whole-NULL
host/FP lifetime rules and genuine-host-fault foreign/default-fatal policy remain.

## Pinned provenance and inspected paths

Wasmtime upstream commit `05e7deebf1ddcc726bfc4c30cd87e3dbb2881072`,
tree `36fbc4207a6101af7544417967dfa26d62f1dea7`, resolved2026-10-13.
Only16 small files were downloaded, about252KiB; no clone/build/runtime dependency.
The root [LICENSE](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/LICENSE)
is Apache2.0 with LLVM exceptions, SHA256268872b9816f90fd8e85db5a28d33f8150ebb8dd016653fb39ef1f94f2686bc5.
Mach source credits SpiderMonkey and links its original implementation; preserve
that provenance if expressions are ported, and route any required license addition
through existing Forge notices packaging, never a new license registry.

| Donor owner | Inspected mechanism / accepted boundary |
|---|---|
| [sys/unix/machports.rs](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/wasmtime/src/runtime/vm/sys/unix/machports.rs)1–515 | One service receives thread exception messages, checks compiled PC/trap metadata, redirects the stopped thread to a landing pad; initialization/drop/fork guard. |
| [sys/unix/traphandlers.rs](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/wasmtime/src/runtime/vm/sys/unix/traphandlers.rs)1–61; runtime/vm/traphandlers{,/signals}.rs | Platform selection and one process owner; nested activation/host-result recording followed by a destructor-free transfer. |
| [sys/unix/mmap.rs](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/wasmtime/src/runtime/vm/sys/unix/mmap.rs)1–250; runtime/code_memory.rs404–479; jit-icache-coherence/src/libc/aarch64_apple.rs | Checked mapping, immutable publication, public Darwin I-cache maintenance before executable use. |
| [fiber/src/unix.rs](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/fiber/src/unix.rs)1–290; fiber/src/stackswitch{,/aarch64}.rs | Guarded stack bounds and explicit architecture context preservation; do not adopt the fiber/continuation abstraction. |

Additional inspected reference files: sys/unix/{mod,signals,vm}.rs and
runtime/vm/stack_switching.rs. Detailed bounded source hashes and retrieval commands
are saved in `build/wasmtime-platform-donor-manifest-v1.md`; snapshots are evidence,
not build inputs. No unrelated crate, Rust runtime or Cranelift is imported.

## Existing-owner platform contract

1. `interpret` retains one recovery/TLS/nesting owner. Linux continues its POSIX
   transport; Darwin hardware guest faults use a private Mach transport under that
   same owner. Keep existing public native entry/result/ranges/suspension signatures
   if possible; do not add an optimizing handler or copy recovery state.
2. Acquire process service readiness and current-thread registration outside a
   writer-held execution attempt. A private, lifecycle-managed receiver has bounded
   resources, explicit stop and join, not a detached or polling runtime. Missing
   readiness/capacity is typed local-unavailable, never a guest invalidity.
3. Capture previous THREAD exception tuples atomically with public
   `thread_swap_exception_ports`: masks, ports, behavior, flavor and owned send rights.
   Nested invocations reuse that thread lease and push the existing recovery state.
   Do not replace task-wide exception ports or serialize guest execution globally.
4. The receiver identifies the registered origin thread and pinned invocation
   record. It must not read its own TLS as the guest's TLS, or add Wasmtime's global
   module registry: ranges and prepared artifact lifetime are already supplied.
   Any private route index is an OS lease index, not a code/map/cache owner.
5. Use SDK `mach/mach_exc.defs`/`exc.defs` via target-scoped MIG generation for IPC
   layouts and saved-route validation/restoration. Do not copy the donor's packed
   request structure/message-number assumptions. Validate message kind, lengths,
   count, origin task/thread and registration; release all received/retained rights.
6. Native recovery requires active invocation + artifact code PC + synchronous
   owned memory/guard address, or a validated arithmetic trap. Defined legacy
   interpreter recovery instead uses its established bytecode/memory-address rule,
   because the host PC is not in the bytecode span. Whole C++ host callbacks are
   inactive regardless of either backend. Arbitrary BAD_INSTRUCTION is not made a
   guest trap from PC alone; the current artifact has no trap-site table and normal
   semantic traps already use its software trap callback.
7. A non-null previous THREAD route is an explicit cold conflict: restore its
   exact mask/port/behavior/flavor and return typed `execution_unavailable` before
   guest entry. Never discard that route or pretend TASK fallback preserves it.
   On admitted threads, foreign faults return failure for the kernel's existing
   task/default path. Preserve the separate POSIX foreign-signal forwarding owner.
   A generic proxy to arbitrary foreign thread handlers is outside this port.
   The compute worker's exception-port lease must not be changed externally
   while active; this is not an interoperability layer for other runtimes.
8. On managed hardware error, public `thread_get_state`/`thread_set_state` and ARM
   SDK PC/LR/SP/FP accessors select a tiny landing pad on an admitted host-owned
   recovery stack BEFORE executing any C/C++ or touching the exhausted guest SP.
   The donor's unchanged faulting SP is not sufficient for our actual guard-fault
   gate. The native caller's existing recovery buffer can supply this reserve;
   the legacy thread lease needs an equally ready, privately owned reserve. No
   opaque jump-buffer offsets, private Apple function or raw sigreturn magic.
9. The raw landing pad performs the current-thread transition barrier and existing
   destructor-free transport only. Darwin checkpoints use initialized save-mask0
   and `pthread_sigmask` owner restoration; EVERY hardware/typed(-1)/explicit(-2)
   transport and normal exit must enter raw continuation with managed signals and
   SIGPROF blocked. No C++ diagnostics, exception temporaries or guard lifetime is
   exposed to rearmed recovery. The receiver does not construct guest exceptions.
10. `invoke_without_signal_recovery` suspends Mach activation as well as POSIX
    destination before the whole host callback/conversion/FP/local catch/nested
    lifetime. Nested guest entry can publish its own activation and restores NULL
    on return. Exceptions are recorded while NULL; guest transport happens only
    after every C++ callback frame returns/unwinds. Genuine host faults stay foreign.
11. Warm invocation teardown deactivates the existing recovery, completes POD-only
    result cleanup, and restores the previous recovery/full current-thread mask
    LAST; it never installs/restores ports or starts/joins the receiver. Cold worker
    release instead retires the registration, restores previous thread routes,
    drains record references and releases rights/resources. Borrowed reserves
    outlive release/join; partial startup relies on same-owner TLS thread-exit
    cleanup. Only unrecoverable OS/resource restoration uses fail-stop.

The private legacy callback remains a restricted guest checkpoint, not a generic
C++ signal/EH guarantee. Darwin no longer needs guest hardware delivery on a POSIX
alternate stack; caller altstack state must remain untouched and enabled/disabled
round trips must prove that fact. The known process-wide libc mask restoration is
not used by the new Darwin transport. Linux SIGBUS ownership does not expand.

The coordinator explicitly preserves the original private **both-empty** legacy
catch-all with an active destination, including real raised managed signals and
its old raw-hardware behavior. This exceptional internal callback contract is
trivial-only, not the new Mach/native known-code whitelist. Actual production
interpreter guest gateways supply code/memory allocators
(`execution_context.cppm` defined-function gateways); their nonempty ranges and
EVERY public native entry use Mach-only hardware recovery on Darwin ARM64. Their
POSIX faults/signals always forward. Whole C++ callbacks have a NULL destination,
so they never enter the private compatibility branch. No rejected-Mach sticky
record, new public catch-all or generic exception framework is introduced.

Foreign POSIX simple/detailed callbacks also execute their COMPLETE C++ lifetime
with the canonical TLS destination NULL, not just a local classification pointer.
They reuse the same existing suspension/exception adapter. Legacy typed failure
uses the already owned outcome after callback cleanup; active native handlers
must return without throwing, and an unexpected exception terminates while NULL
as a C++ host-contract defect, not an OS restoration failure or guest trap.

## Mapping, stack and interruption boundaries

Compiler owner retains `artifact::prepare`/`native_image` as the sole mapping and
descriptor owner. The coordinator's first native CLI lane is RW preparation → R
freeze / RX code, public `sys_icache_invalidate`, then immutable shared publication.
No MAP_JIT arena, unsigned-executable entitlement expansion or Hardened Runtime
support claim is made. Signed/hardened distribution is a separate verified package
gate; failure to prepare must not masquerade as a guest trap. Wasmtime's broader
custom-memory/fiber/pooling/unpublish APIs are not copied.

Executor/native_stack owns one private architecture-specific entry/host-bounce
primitive. Preserve Darwin callee-saved registers and alignment, reserve X18,
admit actual OS thread bounds/headroom, and account generated per-function frames.
C++ host/numeric conversion/nested VM runs on the admitted host stack, not an
unbounded guarded guest stack. Local host/recovery capacity failure is local
unavailable with complete enclosing-attempt rollback, not a new chain depth limit.

Deadline/stop transport remains invocation-owned and does not revoke shared RX
pages. This Mach slice does not change poll ABI or introduce a bounce per loop;
the coordinator owns the separate donor-shaped cheap interrupt fast-path contract.
No per-invocation timer pool or parallel watchdog framework is introduced here.

## Process and source ownership gates

Mach-initialized fork children cannot reuse the inherited receiver/registration.
Use fresh `posix_spawn`/exec test and compiler-helper processes outside active guest
registration; explicitly test inherited dispositions/masks/routes. The atfork child
marker is POD-only and rejects unsafe reuse before mutation; do not recreate a
service in a fork child or call C++ cleanup there. The compiler owns helper lifecycle.

Source scope: existing signals.cpp/cppm + a paired private `mach_recovery.hxx/.cpp`
component in the interpret owner, owning CMake's private/generated SDK glue, focused
native/internal tests and migration note. The only runtime-owned FP-test changes
are the approved mechanical fork-to-fresh-exec harness and filter correction;
compiler owns FP/execution_context semantics. No optimizing artifact/native_image,
Spine, P2P, memory-cache API or new library/framework is changed here.

Before source acceptance: public-MIG message/rights audit; existing thread-route
all-behavior conflict rejection/restoration; nested NULL/local catches; queued PROF and
full sibling masks; real exhausted-SP and memory guard recovery; concurrent128
faults; normal/-1/-2/hardware/foreign exits; setup/teardown failure injection;
fresh-exec fatal-host controls; unchanged full unit/internal/native tests on both
required targets. Current197/19/9/2 passes and native timeout are unchanged history,
not Mach acceptance. The private first slice currently rejects all non-null prior
thread routes (all behaviors/flags), atomically restores them, and returns typed
`execution_unavailable` before the callback. Earlier full-forwarding work was
removed from scope after the user clarified the goal: port Forge EOS-VM-OC to
macOS using Wasmtime only as a platform donor, not build a general runtime
interoperability layer. This does not close RPC identity, rights/lifetime,
executor, package or performance gates. The first combined full CTest failed; after causal owner
and child-filter corrections, all three owning groups passed (unit197/internal24/
native18), followed by three bounded until-fail repetitions of each full group.
Exact status and remaining controls belong in the immutable source/command
manifest, not a blanket Darwin/OC readiness claim.

## First combined failure — exact Darwin POSIX provenance

The unchanged donor `signals_tests_line_23` calls `raise(SIGSEGV)` under its private
both-empty legacy checkpoint. A parent crash report and bounded read-only LLDB
capture of binary `5189b48a70d576c7f10a998db89c94db9fad834ba47add0a32c896c613a8f3cb`
showed an active destination, `strict=0`, both ranges empty, `si_code=2` and address
zero. The Darwin `synchronous_fault(si_code)` prefilter incorrectly forwarded it
to default-fatal before checking the legacy contract. Other faithful runs passed;
they do not invalidate this observed failing branch or close the defect.

Matching public XNU commit `bbb1b6f9e71b8cdde6e5cd6f4841f207dee3d828`
(`xnu-12377.41.6`, the running kernel version) independently explains why the
Linux-style discriminator is invalid: ARM
[sendsig](https://github.com/apple-oss-distributions/xnu/blob/bbb1b6f9e71b8cdde6e5cd6f4841f207dee3d828/bsd/dev/arm/unix_signal.c#L439)
sets SIGSEGV/SIGBUS fault codes regardless of the original sender. This is source
evidence, not a private API dependency. A sticky rejected-Mach flag is not a safe
substitute: a task handler can handle/resume without delivering POSIX, leaving
stale provenance. The approved correction restores only the original private
both-empty behavior and makes native/nonempty Darwin POSIX delivery always
foreign, as described above. Actual raise(SIGSEGV/BUS/FPE), nonempty forwarding,
private hardware, native-empty and whole-NULL fatal controls all executed in the
corrected full owning rerun. This closes the observed prefilter regression's
focused evidence, not the remaining Mach/RPC/executor/Spine acceptance matrix.

Raw capture/script hashes and the isolated matching parent crash report are saved
under `build/mach-internal-handler-live-capture-v2.log`,
`build/mach_internal_handler_capture.py` and
`build/mach-internal-first-crash-summary.json`. The child exited/reaped; no source
instrumentation, donor assertion change or process-wide system setting was used.

An independent first-freeze review then found the local-only NULL forwarding gap:
a nonempty legacy foreign handler could still fault at an owned address while
Mach saw active TLS recovery. The narrow same-owner dispatch correction passed
all three groups (unit197/internal27/native18), plus three bounded repetitions.
New controls cover simple/detailed normal return, NULL observation, complete RAII
and legacy typed cleanup, restoration of the outer destination, genuine owned-
memory callback faults remaining fatal, and native unexpected throw terminating
with canonical NULL. The initial incorrect terminate test oracle is preserved as
a harness failure; no public TLS/query API was added to make it pass.
