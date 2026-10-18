# WAMR Optimizing Checks

This is a bounded development checkpoint, not production acceptance. The
[runtime limitations](../../libraries/vm/wasm/optimizing/README.md) remain
mandatory. The authoritative current status is the
[2026-10-18 R5 integration report](../../docs/donors/forge-wamr-dev-integration-20261018.md).
No interpreter fallback or test-only production path is enabled.

## Current 2026-10-18 R5 Qualification

Current source pin: `12d1f906668cde93c03e7d864e71ee1e10dfb194`.
Fresh Darwin ARM64/LLVM 22.1.8 evidence qualifies the following bounded checks,
not every admitted module shape or production/product acceptance:

| Check | Actual result | Evidence |
| --- | --- | --- |
| Source AOT, live 8/16/32 KiB frames, typed traps, preparation, session/cache and cleanup | 47/47 CTests PASS | [runtime-matrix-r2.xml](../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-matrix-r2.xml) |
| Installed V4 compiler/export-name producer, runtime and real-helper session | 5/5 nested CTests PASS; outer package runner 1/1 PASS, not six independent tests | [package-optimizing-r4.xml](../../../../artifacts/wamr-dev-integration-20261018-r5/package-optimizing-r4.xml), [nested package-tests.xml](../../build/wamr-dev-r4/forge-package-vm-wasm-optimizing-component/package-tests.xml) |
| Current public ARM64 LLVM JIT profiling | Public profile CTest PASS, 9 cases; genuine native compile/bridge/maps checks also PASS | [later runtime-matrix-r4.xml](../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-matrix-r4.xml) |

The R2 public profiling cancellation failure remains in its original XML; the
later R4 result follows the reviewed test-only cancellation fixture correction.
The historical donor unit/spec failures are not green parity and are classified
in the authoritative report. Installed tests use separate compiler/helper and
runtime processes; they do not qualify in-process compiler/runtime composition.
x86 execution, LLVM-free installed runtime and full product integration remain
NOT_RUN. Rebuild actual compiler/runtime/fixtures after identity changes; no old
binary or sidecar is relabelled by this status update.

The sections below preserve earlier source-design and qualification checkpoints.
Their pending/NOT_RUN statements and old counts describe those named snapshots,
not the current R5 result above. Original logs and failed results are retained.

## Historical 156dbd0 Source Pin

Source pin: `156dbd0b6edea3d64b7e21d305913943b60b5f91`. The bounded optimizing
C++ integration and matching generated AOT fixtures are NOT_COMPILED / NOT_RUN
on this pin. Historical `75e8f91`, `66e6be6` and older results below or in retained
logs remain historical; none is a pass claim for `156dbd0`.

## Historical 156dbd0 Compiler And Runtime Design

`wamr_compile_fixture` invokes the public compiler in a separate process and
emits the existing request/artifact codec. Runtime test executables link only
the optimizing target; they do not link LLVM or compiler code. Each explicit
fixture authority denotes controlled compiler output, not arbitrary trusted
bytes supplied by a network caller.

`heavy_fixture.cpp` generates three source-storage budget cases. Every i64
local comes from an unpredictable host call before recursion, stays live across
the recursive call, and is individually consumed afterward. Tests verify all
issued/consumed values, 1023 guest frames and shallow reuse. Unused padding is
not accepted as heavy-frame evidence. Native-frame metadata must independently
confirm that LLVM did not eliminate the live values.

`runtime_tests.cpp` also checks absolute deadline, cancellation, racing typed
host exception precedence, exported imports, caller floating-point state,
identity rejection and an explicit small physical-stack failure. The positive
diagnostic stack is 64 MiB; the negative uses 8 MiB. These are explicit test
capacities, not production defaults or proof of every admitted source shape.

The provenance regression substitutes same-signature AOT bodies and global
initializers, then recomputes the envelope checksum. Default decode still has
no executable authority and preparation must reject it. This checks the trust
gate, not cryptographic authentication of native instructions or safety after a
trusted host deliberately grants authority to foreign bytes.

`trap_tests.cpp` exercises actual unreachable, divide-by-zero, signed overflow,
invalid conversion, table bounds/null, indirect type mismatch, memory bounds and
logical recursion depth. Every successful trap check is followed by ordinary
execution on the same executor. Private indexed functions, unexported imports
and indirect imports exercise the common descriptor bridge rather than an
exports-only lookup. Unknown/auxiliary failures remain operational errors.

## Historical 156dbd0 Session And Cache Checkpoint

`preparation_tests.cpp` exercises real source+AOT loading under the shared owner:
conservative upper reservation, root/workspace refusal, one-byte-short retained
cap, retry, pre-cancel, repeated/different-token calls and concurrent publication.
An exact measured TEST cap is used only to exercise the final seal boundary,
never to plan production admission. Module and weak pins must retain the external
charge after all artifact copies are gone, until final physical control-block free.
These cases are registered for 8/16/32 KiB fixtures.

`cleanup_tests.cpp` is a dedicated serial subprocess using PRIVATE create-time
fault injection, not a public engine API. Real AOT protection refusal and held
native unmap produce at least two physical cleanup attempts. Both a direct
artifact and the real session compute worker must retain a reachable owner and
full reservations, refuse incomplete shutdown, then explicitly retry after
unmap is allowed and restore counters to baseline. A fresh real-helper session
must succeed afterward. A successful module's final pin-drop failure separately
requires bounded common-runtime maintenance after all artifact/module/weak pins
are gone. Fault control allocates no second work queue/cache or background worker.

`session_tests.cpp` uses the existing production session, scheduler, child owner
and real stdio helper. It requires successful HELLO initialization, cold prepared
completion/persistence, warm lookup, duplicate consumers, consumer-only cancel,
restart reload, pinned eviction refusal and subsequent eviction/reload. It also
checks real multi-result/page-cap rejection, wrong hints, tight per-load refusal
with smaller-request recovery, corrupt-cache replacement, opaque orphan
preservation and absence of temporaries. Two distinct requests are enqueued
before either wait with worker_count=2 and sufficient explicit aggregate quota;
both external module pins must survive session destruction. Zero additive limits
exercise the documented per-active-worker resident/inflight quota policy.
An additional real-helper load with `max_tickets=1024` must succeed for an
artifact larger than the retired per-ticket share, reuse ready code and return
both counters to baseline. Ticket capacity affects actual metadata costs only.

The queued-admission regression uses one existing Asio runtime thread and no
coroutine suspension between admissions, so verification cannot retire records
before the usage snapshots. Its separate 512 MiB control must reject the fourth
unverified request without charging it, then stop/drain to the exact baseline.
The three-request positive case retains the same 512 MiB aggregate budget and
uses the observed consumer's stricter 1 MiB request/response caps. This does not
change preparation defaults or claim that every valid module fits.
The existing decode reservation on Darwin ARM64/libc++ is:

```text
W = 1048576 * (96 + 4 * 8) + 8 * 1 MiB + 3 * 1 MiB + 4 * 16384
  = 145817600 bytes = 139.0625 MiB per admitted job
J + T = W + request_charge + 576 + 8 * 16 + 240
base_inflight = (1 worker + 4 pending) * 16 = 80 bytes
three-record upper = 80 + 3 * (W + 1 MiB + 944) = 440601440 bytes
512 MiB - three-record upper = 96269472 bytes
```

The request charge is already bounded by `max_request_bytes = 1 MiB` before
admission. The fixed 576/240/16-byte costs above were checked in the existing
compiled ARM64 session object; they are not portable production constants.
Four decode reservations alone exceed 512 MiB by 46399488 bytes. Tests recompute
`W` from public element sizes/current Raw limits and report the actual aggregate
record charge and unreserved preparation headroom via `session.usage()`, before
any verification. They do not use a guessed input multiplier to admit native
preparation. The two distinct requests use the same queued-account check; this
does not itself prove overlapping physical preparation. These diagnostic/test
extensions await the coordinated incremental build and targeted rerun.
The fresh `156dbd0` fixtures were checked with `wc -c`: the largest artifact
is `heavy-32768.foc` at 722146 bytes and the largest request is 41274 bytes,
both below 1 MiB. The session test also enforces this cap on its fixture inputs.
The former synthetic 16 MiB response/4 MiB request options required 268.0625 MiB
per job, so their second-admission refusal at 512 MiB was legitimate. This is
separate from the default preparation-workspace bug with queued records, which
still requires the production current-headroom clamp. The synthetic resident
test allowance remains explicit; this is not real Spine contract acceptance.

After shutdown, the existing account must retain actual resident charge while a
module or weak control-block pin exists, return preparation workspace, and return
the precise resident remainder only after final weak release. No diagnostic-text
parsing supplies semantic classification. Generated private cache directories
are retained for inspection. These new cases have not yet been run on the combined pin.

The helper keeps its binary protocol on a dedicated descriptor. Unconditional
upstream stdout/stderr diagnostics are discarded by the isolated command, not
mixed into Raw protocol frames or logged with input.

Session CTest registration requires an explicit finite
`FORGE_WAMR_COMPILER_TEST_AS_BYTES`. The local diagnostic used
`1099511627776` bytes because of the measured Darwin virtual-address baseline.
This is not a production allowance, RSS bound, sandbox or automatic increase.

## Build And Run

Configure the normal root with the supported toolchain, optimizing/compiler ON
and tests ON, then build the focused helper/runtime test targets:

```sh
cmake --build build/wamr-aot --target wasm_compiler \
  test_forge_vm_wasm_optimizing test_forge_vm_wasm_optimizing_traps \
  test_forge_vm_wasm_optimizing_session test_forge_vm_wasm_optimizing_preparation \
  test_forge_vm_wasm_optimizing_cleanup -j2
ctest --test-dir build/wamr-aot -R '^wamr_optimizing_' --output-on-failure -j1
```

Never share a build/output tree with a concurrent Ninja or CTest invocation.
After a fork/profile/capacity change, rebuild actual compiler and runtime code
and regenerate the fixtures. Never refresh a sidecar to relabel an old binary.

## Historical 65f974b Checkpoint

The 2026-10-16 Darwin ARM64/LLVM 22.1.8 checkpoint used fork
`65f974bebba846e14e026552aaaa0b898b6f9498`, shared profile hash
`f8a7c360400a71084c9224ce9302965f56e51959136d0f56c99b638535dae7fa` and capacity
hash `68e5a0c0304cf4cae11cd396c4d6f0f61b15ad24c83accfeb1d125b8de06af02`.
Compiler feature overrides were still outside that shared profile, so this is
NOT final feature-layout/identity qualification.

- The original runtime/provenance matrix passed 19/20. Native live frames were
  8312/16504/32872 bytes for the three source budgets. The 32 KiB deep case failed
  with an unknown typed failure despite its explicit 64 MiB physical stack;
  its cause and the complete 32 KiB envelope remain open.
- Four new shared-bridge cases passed: logical depth, private indexed function,
  unexported import and indirect import, including shallow reuse.
- Eight genuine canonical trap cases failed closed as unknown on this old pin,
  which has no structured exception getter. They are permanent failing controls,
  not skipped or marked expected-pass. The reviewed getter pin must be rebuilt
  and these exact cases rerun.
- Real-helper session fail-closed, cancellation, wrong hint and corrupt restart
  passed; multi-result rejection is checked through the existing typed protocol.

The combined graph has 33 cases when the explicit session allowance is set.
Its measured old-pin checkpoint is 24 passing and nine remaining failures, not
a green acceptance claim. Raw logs are retained in the owned build under
`aot-bounded-65f974b-freeze.log` and `session-multi-result-65f974b.log`.

The helper link map `wasm-compiler-stdio-guest-trap.linkmap` contains only the
compiler-runtime archive as WAMR C provider: full_init/load_ex/unload/destroy
resolve to its wasm_runtime_common object, with no interpreter implementation,
native_image or artifact_preparation object loaded. Runtime-only test dylibs
contain no LLVM dependency. This proves only the separate helper image, NOT
same-process compiler/runtime coexistence. Shared feature-profile centralization,
new-fork qualification and independent post-freeze review remain required.

The measured helper SHA-256 is
`11601c9989c591d364a27e6ab1261782b57ce9ed8ff082efcf0298c519e0e258`;
the measured link-map SHA-256 is
`16ab3200698b4ae27b8cf535ca1dbf6fd3b956333ea765173d039f66f81ebe89`.

## Historical Centralized Profile Checkpoint

The shared profile was frozen on 2026-10-16 with SHA-256
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`, fork
`66e6be6c626856758ada54ddac2fc81ae49f5648` and codegen version 5. Configure-time
inspection of all 120 upstream translation units in
`build/wamr-oc-sidecar-66e6be6-usage1/compile_commands.json` found 49 `WASM_*`
definitions, with only `WAMR_COMPILER=0/1` differing by role. The wrapper and
runtime owner also match these definitions. Memory profiling, thread management
and usage-aware allocation are enabled. This does not qualify in-process
compiler/runtime composition.

At the profile-freeze checkpoint the existing warm standalone directory
`tests/vm_wamr/compiler/build-fork-66e6be6-usage1` was actually rebuilt,
not relabeled from the precentralization binary. Its three CTests passed:
source rejection, shared-profile definition replacement and table layout.
`tests-v5-profile.log` records this run. The checkpoint probe binary SHA-256 was
`b11bbb7dae90ce068c9acbf378f7ee2850a2ef3da2f5ac37a91fa78c62e44922`;
the wamrc binary SHA-256 was
`fde87b27d6272da547aa71795a5f7a3df1e327b47ecb2ae90fc580b8b06c4267`.
At that checkpoint the public helper, runtime, native live-frame and typed
guest-trap matrix had not yet been rebuilt/rerun against this exact profile.
Those compiler checks did not waive the remaining designs or qualify execution.

The later common allocator proposal narrows pages to `UINT32_MAX / 65536`
(65535), with initial-page admission and rejection of larger explicit caller
caps; the legacy `UINT32_MAX` sentinel still means the supported profile cap.
The compiler consumes the common `check_profile(options)` overload. A warm
incremental compiler rebuild passes four CTests, adding positive initial=65535,
implicit-maximum acceptance and initial=65536 rejection before codegen admission.
`tests-v5-capacity-65535.log` records this intermediate run. This is not the final
allocator/signed-instance-metadata freeze or a new root runtime qualification.
The standalone path is reused; earlier digests above identify historical stages,
not its current binary. The session test also checks explicit out-of-profile
page configuration through the real helper's typed rejection protocol.

## Historical 66e6be6 Bounded AOT Checkpoint

The owned warm root `build/wamr-oc-sidecar-65f974b` was reconfigured and actually
rebuilt against fork `66e6be6c626856758ada54ddac2fc81ae49f5648`, shared profile
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382` and capacity
`bf6c636d99df90d9a89967ddd7657dacbe92047bfeb8d1d037c33f944d8f5266`.
The historical directory name is not its current source identity. Old logs were
retained. Codegen is 5, host ABI is 3; the approved raw-entry removal is real,
including negative compile-time checks for the method and descriptor pointer.

`aot-66e6be6-profile5-build.log` records the helper and three test-target build.
`aot-66e6be6-profile5-ctest.log` and its XML record **32/33 passing**, with all
12 typed guest-trap cases, provenance, cancellation/reuse and actual helper
session fail-closed checks passing. Session tests also cover rejection of an
explicit out-of-profile page cap. Helper children now have a private scoped
working directory; the existing child owner removes it through compute after
physical reap, including failure paths. Historical root `.su` files are preserved.

The remaining failing case is `wamr_optimizing_32768_heavy`, not skipped or
waived. Its live native frame is 32872 bytes on the unchanged diagnostic
67108864-byte stack. A focused debugger stop at the pinned exception getter
returned `x0=0x0000000b00000002`: status KNOWN (2), native-stack-overflow id 11,
not operand-stack-overflow or allocation failure. The diagnostic test log
`aot-66e6be6-first-depth.log` records 640 completed marks, last guest argument 383,
2620160 issued live values and zero consumed values: the next nested entry
(641) fails before its host calls. Actual failing SP versus cached/assigned
boundary has not been measured. Do not infer a proven frame-underestimate cause,
lower the 32 KiB requirement, or inflate production defaults from these results.

`aot-66e6be6-profile5-helper.linkmap` resolves full_init/load_ex/unload/destroy
to compiler-runtime archive object [50], with no interpreter implementation,
native_image or artifact_preparation loaded. The helper binary SHA-256 is
`1bb95097616cd8e51e7ce84c3fc5437499d80425836ded4891e3cf8b93178e59`;
the map SHA-256 is
`1741fee59fb289530a348e5aa53657345ad41cd03ccbdd99650d059ba4ffaf18`.
This proves only the separate helper, not same-process runtime composition.

The exact reservation blocker remains fail-closed; its proposed reviewed
plan/commit and allocation/mapping hooks are described in the optimizing README.
The full 32 KiB envelope, separate raw-wamrc CLI defect and current consumer
lifetime/timed-run qualification remain open. This is not a full migration pass.

## Publication Operational Oracles

The publication-only source adds six runtime modes to the existing
`test_forge_vm_wasm_optimizing` executable and fixture/compiler graph. With the
existing compiler/helper and finite helper address-space allowance enabled, the
conditional AOT submatrix is **53 registrations: original47 plus six**. This is
not the whole project matrix. New publication graph: **NOT_BUILT / NOT_RUN**;
historical F6 or earlier build results do not qualify these additions.

`operational-oracles.wasm` is one fixed module; its matching request/artifact is
compiled with an explicit fixture mode, the existing source/output limits,
depth1024 and local limit32768. It imports only `env.printui(i64)`. No production,
engine, adapter, ABI, cache, scheduler or classification is changed. Existing
heavy/trap/substitution/multi-result generator paths remain unchanged. Compare
their bytes against the preserved preimages when the parent builds fresh PUB.

The six `wamr_optimizing_oracle_*` cases cover:

- `integer`: all61 MVP integer comparison/numeric/conversion signatures;
  boundary operands, independent explicit-interpreter cross-checks, literal
  wrap/sign/shift values, exact divide-zero/overflow traps and successful reuse.
- `cfg`: seven retained typed merge/loop/return/select bodies, all16 dense
  `br_table` destinations plus defaults, literal expectations and interpreter
  cross-checks. The single-byte signed constant `0x63` means -29, not99.
- `memory`: all14 numeric loads/nine stores, signed/unsigned extension,
  unaligned offset3, raw FP transport, adjacent byte guards, last-valid and
  first-invalid addresses, non-wrapping UINT32_MAX loads, no partial OOB store
  or subsequent host effect; integer/FP globals, grow refusal/zeroing and a
  generated-call growth/new-page visibility path (including its no-grow trap).
  The current public allocator keeps its base stable: this does not prove the
  archived raw-context forced physical relocation primitive. Two distinct
  allocators also exercise the same immutable binding without memory sharing.
  Each executor invocation deliberately
  gets fresh globals/memory, not the old raw-entry persistent context contract.
- `fp62`: all62 public numeric operations through parameters, typed local slots
  and generated-to-generated relay. The public WAMR `execute_numeric` provides
  finite reference results; independent literal vectors check reinterpret NaN
  payloads, signed zero, canonical arithmetic NaNs, nearest rounding despite
  hostile caller fenv, and exact invalid-conversion/overflow traps. Width-aware
  copies never inspect an uninitialized upper32. Caller fenv is checked on
  every AOT invocation, including traps and reuse.
- `benchmark_integer` / `benchmark_memory`: archived WASM function bodies,
  original `(0x345c850000000000, 0x345c850000000000, 1)` arguments and independent
  goldens15065454813286283065 /11713105455123543422, exactly one host effect,
  changed-operand and wrong-checksum controls, then original-operand reuse.
  Their shared module envelope has one initial page and max3 for the unrelated
  growth tests; benchmark bodies themselves never grow and touch only page1.
  This is functional coverage, not old SoftFloat/raw-entry performance parity.

Parent qualification must build a fresh PUB scoped graph, run all53 serially
without weakening quotas, retain original failures/XML and native/request
identities, compare the seven old wasm outputs, and inspect compiler/runtime
provider link maps. No warm F6 graph, old artifact relabel, skip or expected-fail
substitution is evidence for this new source slice.
