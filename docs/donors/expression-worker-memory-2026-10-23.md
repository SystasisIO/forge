# Isolated native CEL memory and package qualification, 2026-10-23

## Scope

This follow-up keeps the pinned CEL profile and exact arithmetic from the
[earlier native qualification](expression-cel-profile-2026-10-23.md), and moves
the real parser/checker/planner/evaluator into a persistent native worker.
The maintainer explicitly selected process isolation because the upstream
allocation paths do not all return recoverable exceptions.

This is macOS ARM64 native work. Linux, Contract SDK/WASM/WAMR, Spine consensus,
Turbine integration and paid external providers are outside this acceptance.
An installed native package is a separate gate from a build-tree consumer.

## Owners and reused mechanisms

| Concern | Existing mechanism | Forge owner |
| --- | --- | --- |
| Parsing, checking, planning, evaluation | Pinned CEL-C++ with its real upstream graph | Private `expression/backend` |
| Allocation/coalescing | Boost.Interprocess `managed_external_buffer` | Private worker `expression/memory` |
| C++ ABI, exception storage, emulated TLS and exit callbacks | Pinned, unmodified LLVM runtime sources/archives | Private worker runtime closure |
| Input/output encoding | Forge Raw and Forge variant values | `expression/worker_protocol` |
| Async socket transport, bounded admission, cancellation and deadlines | Forge Asio gates/runtime, Boost.Asio | `expression/runtime` |
| Child ownership and retirement | Forge child signal registration, Boost.Process async wait | `expression/runtime` |
| Installed dependency discovery and public BMI rebuild | Existing Forge package export | CMake `expression` component |

The runtime is an interface partition of `forge.expression.program`; its
implementation remains paired with `runtime.cpp`. The worker loop and memory
domain are private textual pairs. No new aggregate library, scheduler, allocator
algorithm, expression language, evaluator or wire codec is introduced.

Host lifecycle donor files inspected include
`libraries/asio/include/forge/asio/blocking.cppm`, `libraries/asio/gate.cpp`,
`libraries/asio/notification.cpp`, `libraries/asio/child_signal_registration.cpp`, the existing
TLSNotary native exchange and WASM optimizing compiler process lifecycle.
The expression runtime imports their neutral runtime primitives; it does not
depend on the TLSNotary or VM components.

## Memory boundary

One worker has a fixed allocation pool with a selectable capacity up to 64 MiB.
The domain header, segment manager and allocation metadata consume that same
capacity. The helper owns at most 64 immutable prepared programs. Allocation
failure never falls back to the system allocator. Ordinary anonymous mappings
used by Abseil are served from the same pool; unsupported mapping operations
are refused.

Only the worker links the hidden allocation symbols, CEL and its private C++
runtime. The parent keeps its normal allocator and C++ runtime. Thread-local
runtime storage uses the upstream emulated TLS implementation; exception state
uses the upstream supported single-thread mode, matching the worker's execution
model. Exit callback registration uses the real upstream LLVM libc implementation
whose allocation imports bind to the private pool. A no-op ABI stub is not used.

The final link audit rejects unresolved allocation/VM/TLS/thread creation
escapes, shared C++ runtimes, unexpected shared libraries and extraction of the
parent's Asio/process implementation. It records the genuine linked CEL archive,
static runtime identity, resolved headers, source hashes, link map and executable
hash. The order of actual Mach-O image initializers is also checked: supported
Abseil setters disable stack symbolization and mutex deadlock diagnostics before
private C++ initializers can use those allocation-producing diagnostic paths.

**This is not a process RSS or virtual-address cap.** The worker additionally
limits its native stack to 8 MiB and disables core dumps. Code/images, OS loader,
one-time OS unwinder bootstrap, OS bookkeeping and the parent's bounded control
state are outside the allocation pool. The maximum backing store reserves
64 MiB of virtual address space even when a smaller active capacity is selected.
These exclusions must be included in server-level resource budgeting.

Some upstream OOM paths abort. A separate nonblocking descriptor records a
failed allocation before refusal. The parent distinguishes such exhaustion from
an unexplained crash. A failed dispatched exchange retires/reaps the helper and
invalidates its handles. Retirement-pump scheduling failures stop admission;
physical cleanup remains part of the owner's mandatory shutdown barrier. A request
is never automatically replayed; recovery explicitly creates another runtime.

## Transport and ownership

The parent packs synchronous borrowed inputs directly into a bounded frame.
The worker decodes once and provides borrowed scalar/container views to CEL.
Async public methods take ownership before returning lazy awaitables. A joined
exchange retains those values through physical completion. No per-call owning
activation DTO is copied into the backend.

One runtime serializes exchanges through a Forge gate and admits at most 16
public callers. The absolute deadline covers queued admission, serialization
and transport. Queue timeout leaves the healthy worker usable; timeout or
cancellation after dispatch retires an uncertain execution. Separate runtimes
provide explicit parallel execution and independent pools.

Prepared program state is allocated before dispatch. Successful preparation
publishes its ID to that state on the worker strand before reply delivery, so a
late cancellation cannot orphan a prepared program. Destruction queues the
existing local ID for retirement through one owned pump, bounded by the 64
worker slots. It does not synchronously block an IO thread or create detached
background ownership. The runtime wrapper owns shutdown; program handles hold
weak runtime ownership.

Synchronous calls require a host thread outside the injected IO executor.
Applications keep that executor alive through `shutdown_async()`. Shutdown stops
admission, joins active transfers/retirement and reaps the sole child before
returning. Stopped-executor or foreign child-reaper behavior is not an alternate
supported lifetime model.

## Installed package gate

The fixture performs the genuine configured Forge installation, verifies its
worker and provenance, moves the installation prefix and builds a standalone
consumer from the installed public modules. macOS sandbox rules deny reads from
producer source/build trees and the original install prefix, and deny Bazel
execution. The fixture first proves those denials against real existing files
and the actual Bazel binary. It then configures, compiles, links and runs inside
the same sandbox.

The installed consumer must find its worker under the relocated prefix and its
hash must match the real producer worker. No placeholder archives, edited target
validation, copied producer BMIs, hidden source fallback or consumer-side CEL
build counts as an installed-package pass.

## Validation record

The focused native runtime gates passed on the final formatted sources:
17 public cases and five direct worker cases, CTest 2/2 in 3.90 seconds.
The genuine full-Forge aggregate and installed relocation/runtime gate also
passed. Current machine-readable evidence is in
[the validation directory](../validation/expression-worker-memory-2026-10-23).

| Gate | Result | What it establishes |
| --- | --- | --- |
| Native worker build and strict link audit | PASS | Genuine CEL closure, local allocation/TLS/EH/exit-callback runtime, initializer ordering and reviewed imports |
| Public runtime and direct worker CTest | PASS, 2/2 | Real arithmetic, lifetime, cancellation, OOM, slot/descriptor retirement and explicit recovery |
| Exact arithmetic oracle regeneration | PASS, 1460 cases | Fixture still agrees with the independent Python Fraction oracle |
| Real preparation allocation refusal | PASS | The valid 511-node program with 64 declarations exhausted 256 KiB and 512 KiB pools after successful bootstrap |
| Real evaluation allocation refusal | PASS | The same admitted map activation failed recoverably at 512, 640, 768, 896 and 1024 KiB, and succeeded with sufficient memory |
| Full configured Forge aggregate | PASS | Every enabled archive required by the genuine development installation was built |
| Installed expression runtime consumer | PASS, 10.50 seconds | Genuine install, relocation, denied source/build/Bazel access, public BMI rebuild and execution |
| Linux, SDK/WASM/WAMR, Spine, Turbine, live providers | NOT_RUN | Outside this native acceptance |

The OOM tests require the separate fault marker and a real allocation failure
counter. A bootstrap failure is not counted as preparation evidence. A decoder
crash is not counted as recoverable evaluation evidence. The same source/input
must also run successfully with sufficient capacity. Snapshot assertions alone
are not accepted as proof of enforcement.

The installed gate rebuilt its public module dependencies in 85 Ninja steps
with four jobs. Its consumer links only `Forge::forge_expression`, and passed
exact/wide arithmetic, rounding, typed-error, concurrency and memory-statistics
checks. Explicit sandbox probes denied source, build, original-prefix and real
Bazel access before the consumer configured. The original prefix was then
renamed and absent throughout the consumer build/run. Its installed helper
hash equals the producer helper used by the OOM tests; all three provenance
manifests, source pins and 46 dependency notices were verified.

The linked Homebrew LLVM archives were built for macOS 26 while this producer
used deployment target 15. Linker warnings are retained. Successful execution on
macOS 26.1 ARM64 does **not** qualify older macOS releases.

The genuine full-Forge aggregate build passed after earlier resource stops.
Several transitive modules require over 4 GiB of compiler RSS each. Normal
builds used four jobs; the measured heavy full-Forge target was reduced to one
job. macOS swap growth still exhausted the available disk reserve. Successful
native tests are retained independently; installed execution is not inferred
from them. Source trees, existing SDK installations and previous test data are
preserved. After explicit maintainer approval, one old Spine `CMakeFiles`
directory was removed, recovering 8,953,376,768 physical bytes. Its 795 diagnostic
files were retained separately. The full build resumed with the larger disk
reserve. Installed-package acceptance subsequently passed its separate gate.

An earlier all-target attempt also exposed five PCMs built with conflicting
ambient Boost headers. Recorded Ninja dependencies and compiler module metadata
identified those outputs. Only their generated PCM/object files were invalidated;
the build controller now strips conflicting include environment variables in
the actual child process. Current dependency records contain no
`/usr/local/include/boost` paths. No source workaround or global package change
was made for this environmental failure.

## Findings closed

- In-process CEL arenas could not account for the complete owned allocation
  graph or contain upstream fatal OOM paths. The explicitly approved worker
  boundary contains both ordinary refusal and process termination.
- A static CEL archive alone left shared C++ runtime/TLV and exit-registry
  allocations outside the private pool. Pinned upstream runtime objects and
  static archives now bind those paths to the same allocator.
- Parent transitive dependencies retained six Boost dylibs in the child. The
  supported Darwin dead-dylib stripping option removes them; the actual linked
  closure is checked after linking.
- Symbolization could allocate through system `backtrace_symbols` and free
  through the private allocator. The audited first image initializer disables
  both log and mutex symbolization paths and applies stack/core policy before
  dependency constructors. One-time OS unwinder bookkeeping remains excluded.
- The writer deadline originally began after queue acquisition. One absolute
  deadline now covers admission and IPC without invalidating a healthy helper
  solely because a queued caller timed out.
- Late cancellation could orphan a successfully prepared worker slot. Accepted
  program ownership is now published before delivery, with a joined lifetime
  and a test that observes preparation before canceling and then fills 64 slots.
- Program destruction on an injected IO thread could block its own retirement.
  One bounded retirement pump uses the existing executor and participates in
  shutdown; stale program handles cannot prolong the runtime wrapper's lifetime.
- Malformed response payloads could leak codec/internal error categories.
  Transport corruption now reports worker unavailability; OOM, cancellation,
  deadline and admission saturation retain their separate typed codes.
- The runtime private declaration/implementation pair was split correctly into
  `runtime_impl.hxx`/`runtime_impl.cpp`, preserving the public runtime partition.
- Build-tree worker paths were guessed from the default output directory, and
  installed paths assumed one-level `LIBDIR`. Generated target-file sidecars
  and relative package-to-bin paths replace those assumptions.
- The initial installed fixture checked binary identity but did not bind all
  provenance manifests. It now checks the worker/backend/runtime/pins/notice
  chain before attempting the consumer build.
- Final review found that the installed `expression` component had not added
  Asio, Raw and Boost.Process to dependency discovery. Its component closure
  now includes them. The standalone fixture no longer discovers or links
  Threads itself; it links only the exported expression target.
