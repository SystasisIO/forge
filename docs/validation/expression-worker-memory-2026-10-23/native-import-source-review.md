# Native expression worker import review

Scope: read-only source review plus actual r4 Mach-O imports. Native macOS ARM64 on the current host only. This does not establish a process RSS or virtual-address cap. The pool bounds owned heap allocations; loader, shared system image/stdio/thread/unwind bookkeeping and separately bounded stack remain explicit platform overhead. Static private runtime state (including the initial 32 atexit slots and emergency exception buffer) has fixed image lifetime.

## Actual linked closure

- r4 helper SHA256: `23260b876cf1ae92e5d063af9521ec370b35382e5aa0a9c732660793dddf12b7`.
- `otool -L` now contains only CoreFoundation and libSystem. The earlier six Boost dylibs disappeared with the supported Darwin `-dead_strip_dylibs` flag.
- `dyld_info -inits` first image offset is `0x12DC`; llvm-nm identifies local `_forge_expression_initialize_diagnostics` at `0x1000012DC`, relative to image base `0x100000000`.
- Local `malloc`, `free`, emutls, EH globals and genuine LLVM libc `__cxa_atexit` definitions are present. System allocator, VM mapping, native TLV and thread-creation imports remain independently forbidden by the verifier.
- The newest source moves stack/core/SIGPIPE setup into that same first initializer. Its final order still requires the next link audit.

## Source-bound groups

1. CF and local timezone: cctz `local_time_zone()` calls CFTimeZoneCopyDefault before reading TZ, so TZ=UTC alone is insufficient. No active CEL parser/checker/planner/runtime registration or adapter path calls LocalTimeZone or InitializeLog. CEL FormatTime callers explicitly select UTC. Time overloads/types are rejected before planning. Abseil log timezone pointer is constant-initialized null; its fallback formats raw Unix seconds without resolving a local zone.
2. Symbolization: Darwin Symbolize calls system backtrace_symbols and then free; this would cross allocator ownership. The first initializer disables fatal-log symbolization, sets reported stack depth zero and disables mutex deadlock reporting (a separate direct Symbolize caller). Generic backtrace still performs a one-time OS unwinder bootstrap and may run even with reported depth zero; that OS bookkeeping is outside the allocation-pool claim. No failure-signal handler is installed by the adapter.
3. Classic locale: LLVM libc++ creates classic facets in fixed placement storage. Darwin `_LIBCPP_GET_C_LOCALE` is null LC_C_LOCALE; no named locale is constructed. Actual helper has no newlocale import. Named message catalogs, FILE streams, time and wide-character/floating facets remain linked through virtual tables but the worker exposes no calls that use them.
4. Floating literals: Pratt uses Abseil SimpleAtod, which delegates to `absl::from_chars`; admission then rejects double constants. It does not call system strtod/strtof. Floating Forge variants are rejected by transport admission before sending. Integer/bool/string diagnostics do not invoke floating num_put/asprintf_l paths.
5. Legacy exception ABI lookup: LLVM refstring uses one cached dlopen of the fixed `/usr/lib/libstdc++.6.dylib` with RTLD_NOLOAD, then dlsym for the legacy empty-string singleton. It cannot load a new library. This helper has no libstdc++ dependency. Loader bookkeeping belongs to the explicit OS baseline.
6. Stdio: IPC uses unistd reads/writes. Darwin's three standard FILE objects and their lock metadata are fixed native state; stderr is initialized unbuffered. Fatal stderr reporting does not open FILE streams or catalogs. Named/file/wide stream operations are profile-unreachable; empty iostream initialization wraps existing standard FILE objects.
7. Native pthread calls operate on caller-provided mutex/condition storage and fixed TSD tables. Compiler-rt emulated TLS payloads and its allocation table use the private malloc family. There is one worker thread and no pthread_create import. Apple pthread_tsd.c and pthread_cond.c source snapshots are retained alongside this record.
8. Integer compiler helpers (__divti3, __udivti3, __umodti3), pure libm operations, string/memory primitives and bounded caller-buffer integer/error formatting do not own a growing heap graph.

## Genuine LLVM atexit closure

The unchanged LLVM 22.1.8 libc atexit.cpp and CPP/new.cpp are compiled with upstream public packaging, full-build, single-thread and C-assert modes. Public packaging selects the ordinary growable ReverseOrderBlockStore, not the test-only fixed vector. Standalone combined nm imports were exactly calloc/free. The product runtime manifest records both objects, 115 exact source/header pins plus the libc license, actual compiler depfiles and resolved dependency hashes. Each growable registry block therefore consumes the private pool. Worker exit uses _exit, so it does not call libc's restricted per-DSO finalize path. No new ABI algorithm or fake atexit stub was introduced.

## Lifecycle read-only review

- Prepare preallocates control state and passes its accepted ID slot into the joined strand invocation before reply delivery. Late cancellation either retires the child on exchange failure or destroys owned state and enqueues that accepted ID.
- Pending exchange admission is at most 16; the separate retirement array holds at most the worker's 64 prepared slots. There is one retirement pump. No generic background job queue is added.
- A single absolute deadline covers writer admission, packing and IPC. Malformed prefix/payload and incompatible identity/statistics become worker_unavailable; bad_alloc and fault descriptor markers identify resource exhaustion separately.
- Exchange failures stop, shield physical cleanup and await child completion. Owner shutdown closes admission and waits for pending exchanges/retirement pump before reap and signal-registration release.
- Retirement notification scheduling/allocation failures only make admission terminal; mandatory owner shutdown performs physical reap. This is an explicit bounded idle-child lifetime, not a promise of successful scheduling under parent OOM.
- Direct worker tests cover all 64 slots/reuse, real preparation and evaluation OOM, fault marker versus unrelated EOF, timeout, repeated restarts and descriptor lifetime. These are authored fixtures; their executable results remain a separate acceptance gate.

## Evidence boundaries

Apple source snapshots are current official source inspection, not a proof for older macOS releases. Homebrew LLVM 22 archives were built for macOS26 while the current project deployment target is15; native current-host acceptance does not qualify execution on older systems. Source hashes and raw official-tool outputs are adjacent files. Changes to profile/LLVM/CEL pins, constructor order or exposed types require renewed source review. OS RSS and total system writable-memory bounds are not claimed.

## Final r6 review acceptance (supersedes r4 binary observations)

- Current helper SHA256: `98b296b59301dd95684a474c8f6b66bb7cf40a0ffaa2b9906dd54aff7a785f4b`; it exactly matches the strict worker manifest. Current source, runtime, backend manifests and official-tool outputs are copied beside this record as native-final-* files before build cleanup.
- Strict verifier PASS. Private allocation/new/delete/EH/emutls/atexit symbols in the manifest are all local. Only CoreFoundation/libSystem are shared dependencies. The exact first image initializer is independently identified from dyld_info offsets and llvm-nm addresses. Its current source and manifest include stack8MiB, core0 and SIGPIPE ignore before diagnostic setup and every image C++ constructor.
- Source findings closed: Boost dynamic closure removed; native TLS ownership replaced with unchanged single-thread EH and compiler-rt emutls; genuine unchanged LLVM libc exit registry uses private calloc/free; both fatal-log and mutex symbolization disabled before image constructors. Parent Asio/runtime implementation objects are absent from worker link-map. The verifier rejects both runtime.cpp.o and runtime_impl.cpp.o extraction.
- Focused CTest PASS 2/2 targets: 16 public cases (14 profile +2 runtime fault/configuration) and5 direct worker cases. Raw focused-ctest.log/xml retained. Public late prepare cancellation recovers all64 slots; inflight cancellation with a stopped real worker returns typed cancelled and makes context terminal.
- Genuine preparation OOM refusals observed at262144 and524288 bytes. Genuine evaluation OOM responses observed at524288,655360,786432,917504,1048576 bytes. The same valid activation succeeds with a larger cap; tests also verify post-OOM counter persistence. These are allocator refusals during actual CEL operations, not logical AST/input quota refusals or toy allocator evidence. Raw worker-oom-evidence.log retained.
- Direct worker tests also pass64-slot admission/release/identity reuse, framed overflow error category, allocator marker versus unrelated EOF, timeout,12 restarts and descriptor lifetime. No child survivors were reported by the sole builder.
- Independent lifecycle source review has no remaining blocking findings in this scope. Pending16 and release64 storage bounds, absolute queue/packing/IPC deadline, accepted-ID publication before outer delivery, weak program/runtime ownership, cancellation-shielded exchange reap and shutdown ordering are sound. Terminal retirement scheduling failure retains one bounded child until mandatory owner shutdown as documented. Queue saturation/parent allocator failure injection was not separately executed by this reviewer.
- This review does not stand in for the full Forge suite or installed-consumer qualification; those are running under their separate owner. Native current-host-only and allocation-pool versus OS RSS boundaries remain unchanged.

## Final formatted-source record

The preceding r4/r6 sections are historical source and native-link review captures. The final formatted worker is SHA-256 `de3a3cc989bcf3bb34aa17422e365fdda642941d79eb9b3835c316b447a1e43e`. Its fresh strict link audit passed and every recorded source hash matches. Final CTest covers 17 public cases and 5 direct worker cases, 2/2 PASS in 3.90 seconds; the independent arithmetic oracle contains 1460 verified cases. This includes real public preparation exhaustion at a 512 KiB pool and descriptor cleanup.

The retained files in this validation directory are `worker-manifest.json`, `runtime-manifest.json`, `build-manifest.json`, and `focused-ctest-final.log`/`.xml`. Earlier `native-final-*` names refer to scratch captures under `/tmp/forge-expression-installed-20261023`, not to additional files required beside this report. The allocation-pool/OS-overhead/older-macOS limits above remain unchanged. Installed execution is a separate gate in `acceptance.json`.
