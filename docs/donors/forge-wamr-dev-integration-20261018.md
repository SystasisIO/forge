# WAMR Public Profiling And Delivery Follow-Up

Date: 2026-10-18; latest checkpoint 2026-10-19. Status: pending changes,
NOT ACCEPTED as a complete migration.

This follows the [previous checkpoint](forge-wamr-dev-integration-20261017.md).
The F6 integration branch remains `vm-wasm-wamr-dev-integration-v1`, based on
`3a6e388f29c1535d9d9c642877d416ca7e52e83a`. Earlier worktrees and evidence are
preserved. Workspace evidence for this follow-up lives in
`wamr-dev-integration-20261018-r5`.

## Current F6 Checkpoint

The maintained engine candidate is now the published draft pin
`69c107c54b040b5b0ced5e764f0ddd0c17249029`. The old `12d1f90` sections below
describe their own historical snapshots, not the current pin.

The frozen F6 integration source passed 76/76 selected ON tests in 77.64 seconds,
including interpreter/JIT, public profiling and 47 optimizing registrations.
Source/cache freeze SHA-256 is
`5d828d336ad3afbb56c64e990427141c03e8ab3f67ac7b49891d5f3312f1e609`;
all 311 source hashes and the recorded ON cache matched before installation.
Installed API-stream and interpreter/JIT consumers ran with exit 0; the fresh
installed compiler/AOT/session selection passed 5/5 in 8.55 seconds. Link maps
confirm separate compiler and execution WAMR providers; four SoftFloat globals
retain their TLS ABI. The ON profile deliberately links LLVM 22.1.8.

Spine's isolated installed-consumer build passed 3/3 contract gates in 4.10
seconds, including actual token AOT execution. Its selected real-process CLI
bootstrap, verified-read and restart scenario passed 293/293 assertions in
150.23 seconds. Nine other cases in that executable were filtered, not run;
this is not a 10/10 process suite or AOT-daemon qualification.

The fresh F6 runtime-OFF build/install passed, with 14/14 selected runtime
tests, 2/2 installed AOT/session tests and a direct interpreter smoke. Actual
resolved runtime dependencies exclude LLVM and OpenSSL4; each optimizing leaf
has one OpenSSL3 crypto provider. The external F6 compiler helper and fixtures
remain unchanged, as do all 311 source hashes. Turbine's F6 core rerun passed
29/29 tests in 4.38 seconds, with 674 source, 41 guest and four legacy inputs
unchanged. Its actual runtime compile commands select `69c107c`; this source-mode
interpreter selection is not a full daemon, AOT/JIT or live-product result.
The broader selection is blocked
by an independently reproduced non-VM Clang/libc++ `std::any` error. Linux/x86
and remote CI remain unqualified. The separate publication branch is based on
`05f9edaf`; its results and remaining gates are recorded below.
No live product pin or released default is changed by these local results.

## Publication Checkpoint (2026-10-19)

The clean branch `vm-wasm-wamr-ci-v1` is published as
[draft PR190](https://github.com/SystasisIO/forge/pull/190), targeting `dev`.
No merge or live Spine/Turbine change is authorized by this checkpoint.
The later Swarm commit `4b097418` merges without textual conflicts; this is
not a claim that all Swarm or whole-project tests were executed locally.

Source commit `7d0556d18a8940b9936e6f7bfb5374408353fdbd` passed the fresh
Darwin ARM64, Clang/libc++ 22.1.8, JIT-ON matrix: **55/55 CTests in 60.61s**.
It includes all 53 AOT registrations, the full 301-case unit suite and the SDK
execution suite. All seven previous WASM fixture hashes match; the six new
public-API oracles exercise integer/control-flow/memory/FP and benchmark
checksums. No skipped, disabled or expected-failure cases were used.
The explicit 1 TiB diagnostic helper address-space ceiling is unchanged.

The initial publication build retained an obsolete Mach-header reference;
its failure was preserved and the omitted F6 signals implementation restored.
The retained dev watchdog tests then found an actual nested-cancellation
classification defect. Private transport now retains its live owner across
an intermediate scope's own stop, without hiding genuine host failures.
Regression checks cover body and guest/host paths, exact exception identity,
cleanup/reuse, expired/cross-thread replay and ordinary local cancellation.
The internal failure-scope classifier is additive; no consumer adaptation or
engine-pin change is required by this fix. Direct SDK-test leaf imports also
compile and execute locally; the original remote ambiguity is not declared
closed until the corresponding CI profile passes.

Workspace receipts are under `publication-qual-r3`, `publication-qual-r4` and
`publication-qual-r5`; earlier failures are not overwritten. The final phase
rechecks 5,241 source/dependency paths with six explicitly reviewed overrides,
an unchanged configuration and a clean source tree before/after execution.
Documentation-only descendants do not relabel the tested source commit.

Remote runs48/49 establish real blockers, not runtime passes: exact 22.1.8 apt
packages and the Intel macOS bottle are unavailable; the official Linux X64
LLVM archive passes its hash check but has `RTTI=NO`, rejected by the current
integration gate. The corrected ARM profiles still require a fresh CI run.
Fresh publication link-map/provider and installed/product qualification also
remain separate gates. In particular, this JIT-ON matrix is not an LLVM-free
runtime or performance-equivalence claim, and does not transfer the F6 package
or product results to the publication branch. The migration remains unaccepted.

## Language-Version Differences

The bounded numeric profile uses WAMR's existing modern encoding and typing
rules within its enabled feature set. It does not claim support for all Core
3.0 proposals. In particular:

- `memory.size/grow` accepts index zero encoded as a valid `u32` LEB128 of one
  through five bytes. Nonzero memory indices remain forbidden in this
  single-memory profile. Overflow, an overlong six-byte integer and an absent
  memory are rejected.
- An unreachable `br_table` may target different numeric label types when its
  polymorphic operands satisfy every target. Explicit incompatible operands
  remain invalid.

These are language-version differences from the old EOS test oracle. They do
not require a stricter loader or a second validator. Sources: [memory instruction
encoding](https://webassembly.github.io/spec/core/binary/instructions.html#memory-instructions),
[integer encoding](https://webassembly.github.io/spec/core/binary/values.html#integers)
and [branch-table typing](https://webassembly.github.io/spec/core/valid/instructions.html#valid-br-table).
The historical donor test files and failed evidence remain available unchanged.

Fresh public validation tests exercise both positive rules and negative
controls. Zero-index execution is checked through the interpreter and LLVM JIT,
including unchanged source bytes and the returned page count. The branch-table
case compares the original fixture with explicit incompatible `i32`, `f32` and
`f64` operands, including values that match one but not all branch targets.

| Check | Result | Evidence |
|---|---|---|
| Prepared validation suite | PASS on the new candidate, including `i32/f32/f64` controls | `public-new-r1.xml` |
| Public instruction profiling | PASS; real native-PC sampling and observer/lifetime tests | `public-new-r1.xml` |
| Eight private profile scenarios | 8/8 PASS on the complete pinned runtime | `public-new-r1.xml` |
| Export names containing NUL | Interpreter/JIT suite and separate AOT producer/runtime PASS | `public-new-r1.xml` |
| AOT, preparation, session/cache and cleanup | 47/47 CTest PASS in the repeated broad run | `runtime-matrix-r4.xml` |
| Broad runtime/dependency matrix, including 16 preparation-plan cases | 96/98 PASS, zero skips; NOT GREEN | `runtime-matrix-r4.xml` |
| Installed/relocated interpreter/JIT/profile package | PASS | `package-interpret-r4.xml` |
| Installed executor package | Build and direct executable exit 0 | `package-execution-build-r2.log`, `package-execution-run-r2.log` |
| Installed compiler/AOT/session/cache package | Five nested CTests PASS | `package-optimizing-r4.xml`, nested `package-tests.xml` |
| LLVM-free full Forge build and ordered r3 install | PASS at the pre-numeric/FP/TLS checkpoint | [Build][off-build], [refresh][off-refresh], [install][off-install] |
| LLVM-free installed runtime/session package | 2/2 PASS; ordinary interpreter direct exit 0 | [Package XML][off-package], [interpreter build][off-interpreter] |
| Product processes | NOT_RUN | Separate remaining gate |

The combined focused run is 13/13 CTest PASS on Darwin ARM64 with the source
candidate below. It includes `test_forge_vm_wasm_execution` and the fresh
prepared-validation suite. The older `modern-validation.xml` remains evidence
for the previous pin only. This focused pass is not a green historical oracle,
installed-package result or product acceptance.
The public profiling CTest also passed ten consecutive runs
(`profile-repeat-r1.log`, `profile-repeat-r1.xml`). The repeat count is ten
executions of one CTest, not ten additional registered tests or another platform.

The broad run preserves three failing CTests: a `db.store` SegFault,
14 historical unit assertions and 20 historical spec assertions. Case-by-case
audit classifies 12 unit entries as retired malformed permissions, two unit
entries as million-depth configuration refusals, 18 spec entries as modern
grammar/typing differences and two as the initial-page cap. A preliminary
nonzero-index finding was withdrawn after runtime tracing corrected its
test-to-fixture association: `binary.37` and `binary.42` reject in both modes.
All eight actual binary residuals encode index zero. The raw failing suites
are not relabelled PASS. The former two
embedded-NUL spec failures are absent from the new run. All 47 AOT tests pass,
including heavy frames, configured call depth, typed guest traps, cancellation,
reservation overlap, late cancellation, cleanup and runtime pins.

The later R2 build aligned Forge and its ngtcp2 adapter to OpenSSL 3; their
previous different-major SSL context boundary caused the DB Store crash.
All 118 DB Store Boost cases now pass. R2 exposed a timing race in the profile
cancellation test: its finite guest could finish before the delayed stop.
The reviewed test-only V3 fixture requests the actual stop token after entering
a guest-to-host callback, then requires typed cancellation and successful reuse.
Its exact case passes 84 assertions and the complete public profile suite passes
ten further consecutive runs (`profile-cancel-r3/`). This is not evidence of
pure guest-loop cancellation without a host callback; the existing separate
cancellation cases remain in the broad matrix.

R4 reruns all 98 selected tests with no skips. The only failing suites are the
14 historical unit and 20 historical spec assertion entries classified above.
The additional 16 preparation-plan checks are included, not silently omitted:
R3 had selected their unbuilt executable and is retained as a harness failure.
After building that probe, R4 executes the full selection. The source pin and
capacity identity match the rebuilt runtime/compiler and regenerated fixtures.

## Reviewed Source Candidate

The local maintained-fork candidate is
`12d1f906668cde93c03e7d864e71ee1e10dfb194`. The clean detached `vendor/wamr`
checkout and `ForgeWamrSource.cmake` pin agree. This revision was fast-forward
published to the existing draft branch/PR; it is not fully qualified or merged.
The previous revision and its evidence remain
unchanged. The fresh 33-target build completed successfully at `-j2`; the lower
parallelism is an explicit memory-pressure exception on this 24 GiB host.

The candidate extends the existing loader/compiler/runtime rather than adding a
second implementation:

- Opt-in LLVM instruction maps use JITLink relocation and DWARF decoding. The
  public observer pins the complete preparation owner; no map copy or product
  sampler is added. Plain JIT and empty modules retain unknown-map behavior.
- Export names retain their explicit byte length in source metadata, instance
  lookup, AOT emission/loading and the C API. Empty and embedded-NUL exports stay
  distinct. Import/symbol strings remain strict; the AOT wire format is unchanged.
- Failed preparation, cancellation and unload release the same owners. LLVM
  optimizer/debug-decoder transient heap is not covered by the runtime
  preparation ceiling; do not describe it as a total process-memory limit.

Independent source reviews found no P1/P2 issues in the final profiling and
length-aware export implementations. The public signal-sampling test had a
process signal-mask issue; its v2 fix was independently reviewed. Static review
is not a replacement for the fresh tests.

Current development prerequisites are carried from the recorded Forge dev
snapshot without replacing WASM sources. Their source manifests and patch live
in the evidence directory. Product checks use isolated copies of the current
Spine/Turbine sources; no live product source, branch or vendor pin is changed.

## LLVM-Free Checkpoint Before Numeric/FP/TLS

This later checkpoint includes the canonical resolver and caller-checker fix,
but **precedes numeric execution, the public FP guard and SoftFloat TLS
restoration**. Kepler scoped CLEAR is recorded by the coordinator for this
checkpoint, not for the later numeric/FP/TLS changes. The earlier R4 failures
and original build/install logs remain unchanged; no broad rerun is implied.

- Source runtime: [2/2 CTests PASS][off-checker], zero failures/skips: consumer
  API (15 Boost cases) and oracle transformation. Optional caller checks now
  honor their switches; mandatory profile rejection controls remain active.
- Full LLVM-free `forge` build PASS at `-j2`, followed by checker refresh and
  the ordered r3 install. JIT/compiler/tooling are OFF, optimizing is ON and
  LLVM/Clang package discovery is disabled. See [build][off-build],
  [refresh][off-refresh] and [install][off-install].
- Installed AOT export-name runtime plus helper/session/cache:
  [2/2 CTests PASS][off-package]. [Dynamic linkage evidence][off-linkage]
  contains no LLVM/Clang dylib in either runtime executable; the compiler
  helper remains a separate process, not an in-process compiler/runtime mix.
- Installed ordinary interpreter configure/build PASS and direct executable
  exit 0 (coordinator-reported). [Build output][off-interpreter] is retained;
  [run stdout][off-interpreter-run] is empty and is not itself exit-code proof
  or a registered CTest report.
- Runtime delivery uses workspace evidence prefix `forge-runtime-prefix/`;
  the existing `forge-prefix/` supplies the separate compiler helper and was
  not overwritten. Its helper/config match the [pre-switch hashes][off-helper].
  This does not qualify JIT-ON delivery or the later TLS linkage change.

SHA-256 anchors for the linked workspace evidence:

| Evidence | SHA-256 |
|---|---|
| Checker XML | `01ca02b11f4dc858a5513896c263a53d4d3f770bd375760a23b7b4ccaa9b1bae` |
| Full build log | `32283c0a02987a767de8db722279dd47d41603550beab7ca725dda1732612942` |
| R3 refresh log | `a97dc1b20a70670a4623d4cb31b7187c739349bd1c4787977b4549cd746a7273` |
| Ordered r3 install log | `d232a667bc4d16598aa7fe998f40a6c0dd0d1dad5e48f14752dd8dd19b5cd385` |
| Runtime package XML | `3ef4d878383f4675e41dde785b94a189c7acc437e439a58c2ae7c4a64db81177` |
| Runtime package config | `1bc8356535579ce767440dd1a81a11fc4f120c8dca6296b745fe87472f7685d0` |
| Preserved compiler helper | `dab117bdaee3e96e3c650779ae45526ed12d2d5f80046b68e703be147e8f46fd` |

The relative links below locate evidence in the coordinated workspace layout;
these artifacts are not part of the installed Forge package.

[off-checker]: ../../../../artifacts/wamr-dev-integration-20261018-r5/checker-admission-r1/runtime-off-r1.xml
[off-build]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-build-r2.log
[off-refresh]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-checker-build-r3.log
[off-install]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-install-r3.log
[off-package]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-package-r2.xml
[off-linkage]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-linkage-r2.log
[off-interpreter]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-interpret-build-r2.log
[off-interpreter-run]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-interpret-r2.log
[off-helper]: ../../../../artifacts/wamr-dev-integration-20261018-r5/runtime-no-llvm-fp-before-r2.sha256

## Current API Full ON Checkpoint

2026-10-19: numeric v2 and the public FP guard/SoftFloat TLS restoration are
applied together; independent numeric review is CLEAR, with its P2 finding
closed. The parent completed the full build and extra targets (3342 + 131
planned actions). The selected full ON matrix is **72/72 CTests PASS in 74.27
seconds on Darwin ARM64**, with zero failures/errors/skips/disabled tests and
no expected-failure masking. Independent review matched all 72 unique XML names
to the registry and all **290/290 source hashes** to the unchanged original
freeze. The separate ON cache matches the recorded full-role identity.

This selection includes core, interpreter/JIT suites, nine public instruction
profile cases, native-map/error-thread probes and **47 AOT preparation/session/
cache/cleanup tests**. Unit/spec output reports **288/4856 cases**, not assertion
counts; total executed assertions are not printed. The matrix uses the current
build-tree helper, not the prior compatibility helper. Checked link maps retain
one WAMR C-runtime provider per process: compiler role JIT=0, runtime role JIT=1,
with no contributing old EOS engine. This does not claim all donor utility code
has disappeared.

Evidence: [registry][full-registry], [XML][full-matrix], [log][full-log],
[source-role record][full-role], [full build][full-build] and
[extra targets][full-extra]. The earlier LLVM-free 10/10 runtime checkpoint
(165 unit / 2428 spec cases), R4 failures and historical donor inputs remain
separate, unchanged evidence. Passing the reconciled current oracle does not
rewrite the historical oracle or claim the entire root/previous 98-test matrix
was rerun.

Fresh full-prefix installation is RUNNING at this checkpoint. Subsequent full
installed-consumer/current-installed-helper qualification, Linux/x86, remote CI
and Spine/Turbine runtime/process acceptance remain NOT_RUN. This is bounded
Darwin source/build-tree qualification, not complete migration acceptance.

[full-registry]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-registry-r1.json
[full-matrix]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-matrix-r1.xml
[full-log]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-matrix-r1.log
[full-role]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-source-role-r1.json
[full-build]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-build-r1.log
[full-extra]: ../../../../artifacts/wamr-dev-integration-20261018-r5/api-full-extra-build-r1.log

## Remaining Acceptance

- The R4/package rows above retain their historical snapshot; the later
  LLVM-free checkpoint is separately bounded. Real installed Spine compilation
  exposed the missing current-dev `resolve_options` API. A single canonical resolver now
  replaces the migration-only normalizer, preserves configured identity and
  has independent source review. The LLVM-free backend/package gates pass;
  the current full ON source matrix now passes above, while the installed full
  prefix and product reruns remain separate gates. No Spine
  workaround or limit reduction is accepted.
- The four caller `check_*` switches now pass actual admission with compulsory
  profile rejection controls intact. Current-dev numeric/FP utilities and the
  SoftFloat TLS build prerequisite are restored and pass the bounded Darwin
  source matrix above; full installed-consumer and product qualification remain
  required. The maintainer separately approved
  retirement of exactly six old raw native-signal types and five functions; the
  [release note](../releases/unreleased-wamr-bounded-profile.md#native-signal-api-break)
  records that narrow break without waiving the remaining interruption and
  host-exception contracts.
- Fresh Contract SDK and all five isolated Turbine guest graphs build. Native
  configure uses the R2 snapshot at `33b75b421ac276aec129b88dbb3425386f47a64c`
  plus recorded pending source and mechanical namespace adaptations. This is
  not host execution or daemon acceptance, and later live edits are not included.
- The selected 72-test runtime/compiler/session matrix now passes, including
  complete unit/spec registrations with the reviewed approved-profile oracle
  and its 21 selftests. Unselected root tests and broader platform/product
  gates are not inferred; preserve all historical FAIL logs.
- Requalify installed/relocated consumers after numeric/FP/TLS integration;
  do not reuse the pre-restoration LLVM-free PASS as that later evidence.
- Build and run isolated current product copies with a matching Contract SDK.
