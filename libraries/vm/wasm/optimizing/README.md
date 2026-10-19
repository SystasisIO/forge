# forge_vm_wasm_optimizing

Owning value/artifact component for an optimizing WebAssembly execution lane.
The current source slice provides owned compiler requests, canonical compile
identity, checked pointer-free artifact encoding and explicit immutable native
image preparation. Executor/session integration and LLVM lowering have separate
source/evidence gates; this README does not declare a complete production backend.

Public modules are `forge.vm.wasm.optimizing.compile_request`, `.artifact`,
`.invocation_abi`, `.compiler_session`, `.compiler_protocol` and `.exceptions`. Source target is
`forge_vm_wasm_optimizing`; dependencies are the existing interpreter, Forge
Raw, digest, Crypto random/Hex, Forge Asio and Boost.Process v2. Runtime consumers
acquire no LLVM dependency.

```cpp
#include <utility>

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.compile_request;

namespace wasm = forge::vm::wasm;

wasm::optimizing::compile_request make_request(wasm::interpret::wasm_code bytes) {
   auto request = wasm::optimizing::compile_request{};
   request.wasm = std::move(bytes);
   request.configuration.target = wasm::optimizing::target_profile::native();
   request.configuration.execution = wasm::interpret::resolve_options(wasm::interpret::default_options{});
   request.configuration.limits.max_generated_bytes = 16U * 1024U * 1024U;
   request.configuration.limits.max_function_stack_bytes = 16384U;
   return request;
}
```

Products provide their actual execution snapshot and exact registered host
signatures, not current source-admission policy for historical code. Canonical
identity hashes the WASM digest and one described configuration through the
existing Forge Raw/digest implementation. Import ordering never uses process
registry indices. Source/request bytes are private and must not be logged.

An artifact retains a digest/configuration, not original WASM. Its copyable
handle pins the same checked payload and one prepared mapping. `from_payload`
and `decode` do not allocate executable memory. Explicit `prepare()` runs
outside product writers/strict invocation paths; preparation is supported for
the exact Linux x86-64 and Darwin ARM64 profiles. A host/profile mismatch fails
before allocation. Both profiles use normalized ELF input; the explicit triple,
CPU, features and LLVM/codegen/host-ABI versions remain in the Raw identity.
It publishes only after all relocations, public Darwin instruction-cache
maintenance where required, and RW-to-RX/RO
transitions complete. Failed preparation publishes no ready state; retries
are explicit session decisions. `is_prepared()` and prepared accessors never
decode or map lazily. Immutable code is never made inaccessible for cancel.

Borrowed `image()`, `native_code()` and `functions()` views remain valid while
any artifact copy is pinned. Ready caches must index only prepared artifacts;
there is no separate executor mapping cache. `byte_size()` includes owned
metadata/encoding and, after preparation, the native allocation. Code-cache
policy and disk durability belong to the session/product, not this value API.

`prepared_byte_size()` reports the exact prospective value of `byte_size()`
after successful preparation: the same counted metadata/encoding plus the
single native planner's full mapping reservation (including alignment reserve)
and exact-sized native function table. It does not map, protect, prepare or
publish; it may allocate temporary planner storage and throw validation/budget
errors. Host/page compatibility is still checked by actual preparation, not
promised by this portable estimate. Accounting excludes allocator/object
management overhead and is not process RSS. The private native function table
uses fixed-size owned storage; its public immutable span is unchanged.

`is_unique()` is an advisory shared-pin snapshot, useful only with external
cache/index exclusion. It is not synchronization or an eviction lease and never
permits mutating/unmapping code. Existing artifact copies alone pin lifetime.
Pinned cache entries must remain indexed/charged rather than being remapped or
treated as freed because a sampled reference count changed.

The single standard-layout invocation ABI contains only numeric/POD fields
and noexcept raw-bit callbacks. Compiler lowering derives offsets/signatures
from those exact declarations. Stop/deadline/exception objects stay in the
invocation owner, accessed through poll/callbacks; no native FP substitution,
GS register state, STL layout or process pointers enter persisted artifacts.

Artifact format is version 1 with an integrity checksum, not an authenticated
signature. The cache/helper trust boundary must protect native bytes. Unknown
sections/relocations, overlaps, invalid types/start/table/data and stale
identity are rejected before mapping. The existing Raw encoded-size ceilings
also apply; exceeding local budgets is local compilation unavailability, not
a verdict on a blockchain transaction or historical WASM validity.

The Darwin ARM64 profile is `aarch64-apple-macosx11.0.0-elf`, CPU `generic`,
features `+fp-armv8,+neon`; the default Linux profile remains
`x86_64-unknown-linux-gnu`, CPU `x86-64`, empty features. Profile strings are
exact and closed; aliases or reordered feature strings are unsupported.
AArch64 instruction relocations validate opcode, scale/alignment and range
before mapping; no implicit GOT, external-symbol resolver or second loader
exists. The existing mapping is written once, then code is RX and data is R.
Ordinary CLI executable-memory evidence does not establish hardened-runtime
entitlements, signed application packaging or native fault-plane acceptance.

## Compiler session and helper (pending integration)

`session_options` requires explicit finite memory/job/helper limits and
`max_disk_bytes`/`max_disk_entries`. Construction allocates bounded owner
metadata but performs no filesystem, process or SIGCHLD setup. Before admission,
await `initialize(target_profile::native(), stop)`. This eagerly submits one
tracked control record before returning its lazy waiter; discarding that waiter
does not discard the job or the obligation to drain it. Repeated/concurrent
initialization is typed `asio::exceptions::invalid_state`. Failure leaves
admission closed; `enqueue()` returns `not_initialized`, and `ready()` returns
null for cold/initializing/failed/stopped owners. There is no retry hidden in
enqueue. Create a new owner only after explicitly draining the previous one.
The lazy waiter pins only an independent terminal outcome/notification, never
the job, child, task handle or old runtime executor. A canceled pending task can deliver its error immediately,
but only the owner run/shutdown finalizer joins its stop registration and
releases its live job. After successful shutdown, a retained waiter may consume
that outcome on a new runtime or be discarded after the session AND old injected
runtime/compute storage are destroyed. Its conservative initialization workspace
charge stays with the last outcome pin; it is not released while that owned
state is still retained. Outcome publication/registration finalization is
once-only, but retirement of a non-quiescent retained live record is retryable
after the SAME child is physically reaped/drained. A later shutdown retry never
changes an already published terminal outcome.

Initialization validates the SAME full canonical host target before touching
the cache, uses a real HELLO-only helper operation to check exact build/target/
resource limits, requires ACK/EOF and sole physical child reap, then acquires
exclusive cache ownership and completes the bounded writable/scan preflight.
No fake WASM, parser or LLVM initialization is needed for this operation.
`target_profile::native()`, `matches_host()` and canonicalization share the
same closed target definitions. They do not promise mapping availability,
hardening or arbitrary CPU/feature compatibility.

`compiler_session` is the single request-to-prepared-artifact owner, injected
with the existing Forge Asio runtime/compute executor. Its one bounded numeric
priority scheduler owns verification/helper jobs; the canonical index is not a
second queue. `enqueue()` reserves owned capacities/counts and returns a
move-only `accepted_unverified` ticket without hashing WASM, disk I/O, planner
work or compiler waits. Worker-side canonicalization/identity verification
precedes any canonical-index, disk-cache or helper operation. A wrong hint
fails only that ticket, never erases or poisons another canonical entry.

Verified aliases attach to the existing canonical completion and release their
verification scheduler slot. Only a verified leader enters the helper path.
Every successful completion contains the SAME prepared artifact pin, checked
identity/configuration, original leader queued time and completion time/status.
Ticket cancellation cancels only its consumer; existing `asio::notification`
owns wake-before-wait, timeout and stop behavior. `ready()` is immediate,
in-memory and requires full expected configuration equality. Jobs, tickets,
decode workspace, index metadata and prepared allocations are bounded and
charged; ready mappings with external pins stay indexed/charged. These owned
byte estimates exclude allocator bookkeeping and are not process RSS limits.

The session persists the existing artifact codec in a private owned cache
directory: random suffix from existing Crypto/Hex, exclusive atomic-CLOEXEC
`openat`, write/fsync, rename and directory fsync. Restart reads the SAME
identity/configuration/checksum validator before the SAME explicit preparation;
there is no alternate codec, per-call mapping cache or orphan-file sweep.
One nonblocking exclusive `owner.lock` covers the WHOLE directory until all
tracked child/compute/scheduler callbacks have drained; its inode is never
unlinked/recreated by the session. Quota/accounting assumes EXCLUSIVE cooperating
access: all writers obey that whole-directory advisory lock and the private
0700 directory is not edited behind its owner. Noncooperating same-UID changes
while owned are UNSUPPORTED, not a security/sandbox guarantee. There is no
timestamp observer, background reconciliation or external-edit watcher.
Path/lock/selected-file/destination ownership mismatches fail closed; after
successful shutdown a new owner reconciles legacy/external changes at startup.
All filesystem scans/decodes/evictions run
on compute, not under the index lock or product writer. ALL directory entries
consume the finite scan-work bound (dot entries excluded); excessive legacy
contents fail before eviction/mutation, rather than trigger an unbounded sweep.
Payload byte lengths and entry slots include opaque files, crash orphans,
logically quarantined artifacts and live temporaries. The one verified
zero-byte lifetime lock is fixed metadata, not a payload slot. Unknown files
are charged/preserved; unsafe nonregular/foreign entries reject preflight.

Recognized canonical names are rechecked through the SAME checksum/identity/
profile validator on restart. Corruption is only a state in this bounded
catalog, not another codec/sidecar/negative cache. The complete bounded scan and
candidate validation runs ONCE during initialize; that SAME catalog owns warm
accounting thereafter. Load rechecks only its selected file through the existing
inode/owner/length/checksum/identity/configuration validator before preparation.
A rejected selected payload remains logically quarantined/charged for rebuild;
load/persist do not reread/decode unrelated artifacts. Own publication, temporary
cleanup, recency and confirmed eviction update the SAME catalog/charges.
Metadata lookup/eviction remains bounded linear work, not a throughput claim.
Validated candidates can be
evicted deterministically (quarantined first, then timestamp/name); opaque
files never become deletion targets. Replacement reserves BOTH previous
destination and full new temporary until rename, namespace fsync and close
are confirmed, and cannot evade that quota by deleting its destination first.
Failed/unconfirmed unlink or close never releases the charge; disk ownership
is faulted and subsequent writes/scans fail closed until explicit drain and
new-owner reconciliation. Quotas count encoded named-file lengths, not
filesystem block overhead, allocator overhead or RSS.

Disk eviction never invalidates an artifact pin: preparation already copied
the checked image into the SAME independently owned anonymous mapping, after
closing the cache read FD. Existing pins alone own code lifetime; there is no
extra disk pin, per-call mapping or cache resurrection mechanism.

One fresh `posix_spawn` helper has a minimal environment, isolated descriptors
and inherited AF_UNIX endpoint, not a listener or a generic process subsystem.
The private Raw exchange v2 is request (or profile-only preflight) -> actual version/build/target/resource
hello -> exact header-only acknowledgement -> EOF -> compile -> reply. Invalid
ack/trailing input cannot enter LLVM. Stderr never contains request bytes.
Parent socket SIGPIPE safety does not change process-global signal state.
Darwin's public SDK lacks atomic socketpair CLOEXEC; sources are immediately
duplicated with F_DUPFD_CLOEXEC and closed, and OUR spawn uses CLOEXEC_DEFAULT.
The small creation-to-dup window is not isolation from a foreign concurrent
unsafe fork/exec. Linux uses atomic SOCK_CLOEXEC plus public spawn closefrom.

Unmanaged Boost.Process v2 async wait is the sole child reaper. The embedding
application must establish default SIGCHLD, no SA_NOCLDWAIT/competing reaper and
keep runtime/compute pool, executor and storage alive through cleanup. On Darwin,
the first cold session uses the shared `forge.asio.child_signal_registration`
guard and requires exact native default (`SIG_DFL`, zero flags, empty mask);
custom flags/masks on a default handler are rejected without mutation. The guard
uses the existing public Asio signal set. Other sessions, including sessions on
distinct runtimes, may join only while
known own registrations are live and the captured handler, flags and mask are
unchanged. Each session retains its own runtime-bound registration until its
child/task/compute drains. Checked public unregister failure or an observed
foreign disposition change returns `shutdown_incomplete`, preserving the SAME
registration and runtime; quiescence is not claimed. The last successful Asio
unregister restores default disposition. Failed startup after registration also
requires explicit shutdown before destroying injected owners.

VM boundary exceptions remain `compiler_unavailable` for acquisition failure and
`shutdown_incomplete` for release failure. The stricter cold profile changes
which host dispositions are admitted; public error categories stay unchanged.

This is ownership coordination, not a signal handler or reaper replacement.
Child waits remain exact-PID Boost.Process operations. The shared guard's private
mutex serializes only cooperating Forge registrations; it cannot synchronize unrelated
signal mutation or foreign waitpid calls. An embedding host must not replace
the handler or introduce a competing reaper while registrations are live.
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

All helper CPU/address-space/stack limits are explicit finite configuration.
Address space is an ABSOLUTE RLIMIT_AS, strictly set/read back or rejected; no
automatic increase/unlimited fallback or hard-RSS/sandbox claim exists. On
the measured Darwin host, linked-helper initial VSZ is about 415 GiB before
request/LLVM initialization, and the existing parser reserves another 1 GiB.
The trusted tiny-fixture smoke allowance of 1 TiB is diagnostic only, not a
production default. Real deployment calibration and bounded-growth controls,
Linux native/helper tests, installed consumers, allocation/cancellation
injection, saturation/priority and performance remain separate gates. The seven
initial codec/socket/real-helper/session-reconstruction/deadline smoke cases do not establish
the full executor or Spine product path.
The restart smoke reconstructs a second session in the SAME fresh test process;
it is not a fresh-exec/application-restart cache gate. Literal helper spawn
counts and stop/invalid-ack cases at each handshake boundary were NOT_RUN in that
historical packet. The current startup/cache follow-up adds normal-root
registered source controls for fresh-exec restart/HELLO-only plus zero COMPILE,
spawn counting/warm hit, protected old+temp quota, charged unknowns, stable
exclusive lock, external mapping lifetime, failed admission and stopped-compute
startup/spawn cleanup. Those NEW controls are currently UNBUILT/NOT_RUN; prior
green binaries cannot establish them. The separate lifetime remediation adds
genuine successful retained-waiter consume/drop after all old owners, immediate
pending cancellation delivery plus retained consume/drop, and a Darwin-only
real-child first-reap/first-cleanup waitpid ENOMEM control followed by disarmed
successful SAME-record shutdown retry and post-cleanup ECHILD. The waitpid
control does not demonstrate constructor bad_alloc, fake child ownership, or
cache-lock acquisition before the HELLO failure. These controls remain NOT_RUN.
The separate cache-cost source follow-up adds an isolated exact-inode public
read/fstat observation: genuine unrelated artifact reads at startup, then zero
unrelated reads during selected load/new-identity persist; selected checksum
corruption still cannot evade old+temp quota or publish ready. Existing restart,
unknown-entry quota and external prepared-pin controls are retained. These NEW
controls and current-library reruns are UNBUILT/NOT_RUN, not performance PASS.
In-flight disk-syscall failure injection,
all ACK boundaries, fresh Linux helper execution and installed/Spine/performance
gates are not claimed by this source slice.

### Permanent local acceptance wiring (source pending; not yet run)

The normal OC-enabled root test graph owns one fresh fixture setup per CTest
invocation. Build the existing compiler/session/resource/executor/ordinary
consumer targets first. The setup selects the two existing test-only exporters,
writes into a new private nested generation, and requires exactly 18 pairs
(36 files). A single JSON path/digest receipt binds the source revision, source
content, compiler executable and every request/artifact payload. The SAME
compiler test target first writes a target-owned POST_BUILD stamp (declared as
a BYPRODUCT) after its normal CMake dependency build. Source stays frozen for
that owning build. Setup/consumers verify the stamp; they never rebuild or bless
an old executable with newer source bytes. A missing/stale stamp fails with the
existing compiler-target rebuild instruction. On the supported Ninja graph a
script/HEAD-only edit can leave that target otherwise up to date: for a stale
binding, remove ONLY the exact owned `tests/vm_wasm/compiler-build-$CONFIG.json`
POST_BUILD byproduct identified by the diagnostic, then build the SAME compiler
test target normally with `-j4`. Never use `--clean-first` or delete other output
or historical evidence. CTest executes neither recovery command. The actual
generated link edge/missing-stamp regeneration remain NOT_RUN until the owning
normal-root configure/build. The bounded source fingerprint
includes compiler, optimizing/artifact/ABI, interpreter/parser/validation, Raw,
core/exceptions and SoftFloat owners, exporter headers and relevant CMake source
plus configured compiler/flags/system/sysroot identity. It is a test build
binding under normal CMake ordering/frozen inputs, not a new dependency graph
or a security attestation of external SDK/system/dependency libraries.
Every session,
resource, genuine executor and ordinary consumer reads and validates that SAME
receipt; CTest fixture ordering alone is not path transport. A failed setup
invalidates only its own small receipt before starting, never supplied directories
or historical/partial evidence. The receipt expires after one hour and rejects
changed source/compiler/payloads. One invocation owns the build/output tree;
concurrent CTest invocations sharing that tree are unsupported.

`FORGE_VM_WASM_OPTIMIZING_SESSION_FIXTURE_DIRECTORY` remains an opt-in reviewed
reference: both add-profile pairs must exist and every supplied recognized
fixture must match the fresh generation. Consumers never silently substitute a
stale reference directory. `FORGE_VM_WASM_OPTIMIZING_HELPER_TEST_AS_BYTES` remains
explicit finite test input: zero fails the session/resource/package gates, and
there is no host default, automatic raise, RSS promise or resource-policy change.

`test_forge_vm_wasm_optimizing_executor_compiled` permanently selects the existing
disabled `optimizing_executor_compiled/*` suite and requires its actual ten-case
success report. Missing bytes, an empty selection or export failure fail the gate.
The runtime target still has no LLVM dependency. The ordinary public consumer
retains its stop-callback/add/cancel/drain oracle.

The single existing helper install rule is in `COMPONENT dev` without
`EXCLUDE_FROM_ALL`: both the existing dev SDK install and normal all-component
installation include the real helper, without a duplicate install rule.
`test_forge_package_vm_wasm_optimizing_component` uses that existing install
owner plus a fresh, nonoverlapping owned relocation/build/cache directory. It
builds the SAME public consumer with LLVM package discovery disabled. The real
helper is ONLY the relocated prefix's bin/forge-wasm-compiler. The consumer
performs public initialize/HELLO, admission, real compilation, checked prepared
completion/ready lookup and native add execution; a missing-helper initialization
negative must fail closed. Every failure takes explicit session/executor/scheduler
and compute drains with the original owners retained. Historical output is
preserved, not recursively removed. External helper dylibs must still be
provisioned on the host; this does not claim a portable bundled LLVM deployment.

Local registration is not test acceptance: this new wiring and the extended
installed consumer are NOT_RUN until the coordinated normal-root build/CTest
and relocated execution succeed. No Spine code, workflow, artifact ABI/codegen
identity or frozen compiler/exported evidence is changed by this slice.
