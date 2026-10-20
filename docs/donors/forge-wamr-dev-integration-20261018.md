# WAMR Public Profiling And Delivery Follow-Up

Date: 2026-10-18; latest checkpoint 2026-10-20. Status: pending changes,
NOT ACCEPTED as a complete migration.

This follows the [previous checkpoint](forge-wamr-dev-integration-20261017.md).
The F6 integration branch remains `vm-wasm-wamr-dev-integration-v1`, based on
`3a6e388f29c1535d9d9c642877d416ca7e52e83a`. Earlier worktrees and evidence are
preserved. Workspace evidence for this follow-up lives in
`wamr-dev-integration-20261018-r5`.

## CI58 Follow-Up (2026-10-20)

[VM run58](https://github.com/SystasisIO/forge/actions/runs/39293832722) uses
published Forge head `3bcbf790` and WAMR `f6fe6d5`. Its actual checkout is the
synthetic merge `9cd98e54`, parents dev `94657d50` and the PR head, not an
identical copy of the published head. VM/SDK sources and all vendor pins match;
other dev APIs and the combined build/package graph belong to that merge context.
Independent raw-artifact review confirms ARM JIT/AOT **82/82**, ARM interpreter
**25/25**, and Intel interpreter Debug/Release **27/27** each. No CTest failures,
disabled or skipped cases occur. The ARM lane includes the unchanged original
8193-instance case, ten profiling cases with live guest-PC unwind, 53 standalone
AOT registrations and nested installed compiler/session 5/5. The Intel numerical
process failures and provenance mismatches from run57 do not recur. CI58 finishes
with a failed Linux source producer. Stage B LLVM build/install and Stage C
configure pass; final compiler-rt compilation fails in libFuzzer's nested libc++
graph. No qualified toolchain artifact is published. All five dependent Linux
jobs fail at that missing artifact before Forge/WAMR configure or execution;
native, sanitizer and package runtime acceptance remain NOT_RUN, not VM failures.

The follow-up removes Stage C's global Stage A header override, which otherwise
survives the nested libFuzzer libc++ build's `-nostdinc++` and breaks C-wrapper
`include_next`. Before configure, the existing header checker now verifies the
newly installed driver, actual libc++220108 headers and resolved prefix ownership.
Stage B, all three linker/LLD settings, RPATH, libFuzzer, sanitizers and final
relocation/ABI admission remain unchanged. Metadata tests pass **60/60**; a real
Darwin header-search witness fails with two roots and passes with one. This is
not a successful Linux build: the patched producer and its dependent lanes need
fresh remote qualification.

Fresh exact-head Darwin runtime-OFF installation and physical relocation pass
**27/27** root tests, **2/2** direct installed smokes and **2/2** installed
AOT/session consumers. Recursive dependencies for four runtime executables and
actual loaded images of both optimizing consumers exclude LLVM/Clang; each has
one intended OpenSSL3 crypto provider. The separate compiler prefix deliberately
contains LLVM and is not included in that runtime-free claim. Immutable source,
prefix and fixture hashes are checked; all 787 ON cache keys restore unchanged
and the restored 83-test ON selection passes. Independent review is scoped GO
for this OFF delivery, not full migration or product acceptance.

The retained first OFF attempts include a harness SONAME false positive and a
genuine helper/runtime revision mismatch. Apple's exact system
`/usr/lib/libcrypto.46.dylib` was identified through version APIs and `dladdr` as
LibreSSL 3.3.6, not OpenSSL4. The harness now distinguishes that platform image
from the intended OpenSSL3 provider; forbidden-ABI controls remain enforced.
The old preserved helper reports `2d4c3cab` while runtime reports `3bcbf790`;
session correctly rejects it with `4040006` before the compiler-work ACK. A new
normally built/installed matching helper passes without changing the identity
guard. Neither failed attempt is overwritten or relabeled as a pass.

Current Spine/Turbine verification is being prepared from the coherent CI58
merge tree. The exact PR head lacks three newer HTTP APIs required by current
Turbine; individual API copies or compatibility stubs are not used. Product
sources/pins remain unchanged except the approved eight-file execution-module
rename in the isolated Turbine copy. Fresh full Forge tooling/RocksDB install,
matching SDK, donor contracts and real-process effects remain open. Workspace
raw evidence, reviews, immutable archives and approved historical-build
retirement records are under `wamr-ci-190-20261020-r10`. No merge or live product
change is performed; performance, full bounded-profile and final delivered
provenance requirements remain open, including retained EOS-derived utilities.

## CI59 And Current Public Follow-Up (2026-10-20)

[VM run59](https://github.com/SystasisIO/forge/actions/runs/39322192883) is
complete at published head `ae50b6f8`, actual synthetic merge `a92eb494` with
dev `94657d50`. Independent archive/registry/XML/LastTest review confirms
ARM JIT/AOT **82/82**, ARM interpreter **25/25**, and Intel interpreter
Debug/Release **27/27** each, without failures, disabled or skipped CTests.
These results do not qualify the later context-owner or typed-admission fixes.

The previous nested libc++ header failure is resolved: Linux Stage C header
admission, build and install all pass. Publication now stops at the relocated
ABI checker: `HARNESS_ERROR: ASan/UBSan compiler-rt archives missing`. The
standalone compiler-rt build installed those archives under `prefix/lib/linux`,
outside the installed driver's resource tree. The checker correctly refuses
that unusable layout. All five dependent Linux lanes fail downloading the
unpublished qualified artifact before Forge configure; their VM runtime gates
remain **NOT_RUN**, not observed interpreter/JIT/AOT failures.

The CI-only follow-up derives the existing, canonical prefix-owned resource
directory from that installed Clang and supplies its prefix-relative path as
the supported `COMPILER_RT_INSTALL_PATH:PATH`. Explicit cache typing prevents
CMake from converting an untyped relative input against the configure working
directory. The official LLVM22.1.8 install mechanism retains `lib/linux` below
that resource root. Stage A/B, pins, layout choice, linker/LLD/RPATH settings,
relocation and real sanitizer archive/compile/link/run checks are unchanged.
There are no manual runtime copies, symlinks, search fallbacks or driver
resource overrides. Metadata tests pass **71/71**, including all sixty earlier
cases; a real CMake cache probe accepts the typed form and rejects the untyped
negative control. A fresh real Linux producer and its dependent jobs remain
required. Evidence: `wamr-ci-190-20261020-r10/linux-source59-review-r1` and
`linux-runtime-layout-fix-r1`.

Separately, exact clean public head `a06624ff` with WAMR `f6fe6d5` includes
both reviewed runtime corrections below. A normal full producer/install and
the complete selected JIT/AOT registry pass **84/84** in 126.41 seconds. All
three installed VM component consumers pass, including the five nested
optimizing/session checks. Source, helper and provider-role evidence receives
independent scoped GO. This is an ON/JIT snapshot, not LLVM-free delivery or
product acceptance. Additional installed interpreter/execution physical maps
remain **NOT_RUN**; older OFF evidence retains its earlier revision scope.
Evidence: `wamr-ci-190-20261020-r10/public-current-a066-r1`.

At that checkpoint, live dev `8afc1977` added shared child-signal handling and
optional TLSNotary support; the read-only merge simulation was not integration
acceptance. On 2026-10-20 the maintainer authorized merging only the experimental
branch with live dev `c594539f5ea3ec5a73a11bb17dd465fe3f04fdd5`. The integration
retains both WAMR and CJOSE gitlinks, independent optional compiler/JIT,
TLSNotary and JOSE switches, the current leaf/package boundaries, and the shared
`forge.asio.child_signal_registration` instead of a second VM signal registry.
The optimizing README keeps the WAMR artifact and preparation contracts while
incorporating the shared guard's checked acquire/release lifecycle.

Those checks were pending at the integration source checkpoint. The separately
sealed `a66868d62999643cba4882fc4d32d1cd5d21a070` ON/JIT run now passes the full
normal producer/dev install, **84/84** selected JIT/AOT CTests (119.36 seconds),
all **three installed VM component gates**, including **five** nested optimizing
checks, and the shared child-signal **one CTest/five Boost cases**. The same
source remains frozen throughout; the installed helper carries that exact
revision and strict HELLO equality is unchanged. Five physical link maps retain
one provider each: 33 compiler-role C objects or 48 execution-role C objects,
never both. Evidence: `wamr-ci-190-20261020-r11/base-refresh-r1`, postrun receipt
SHA256 `2d83843c830779707ac47375bdf2d720e49b663c63c24c277759a74c49d002dd`.
This is selected Darwin ARM64 ON qualification, not new OFF or product proof.
The first XML audit parser failed on the local Homebrew Python/expat linkage;
its partial capture is retained. The unchanged XML audit succeeded with explicit
system Python3.9.6, without package/DYLD changes or rerunning/relabeling tests.

The independent repeat at exact `b9557e72bb9e9b3a1b2cdff597ab691c84f295cc`
also passes the same **84/84** (115.91 seconds), installed **3/3**, nested **5/5**
and shared-signal **one CTest/five cases** with a freshly rebuilt matching helper.
Its separate postrun receipt is
`452140c5c70daa8e60d6a72de6dbdeed3429d660a130c62d38d20fb10b8de1d1`;
evidence: `wamr-ci-190-20261020-r11/base-refresh-r2`. This repeat has independent
evidence GO, still scoped to Darwin ARM64 ON and not Linux/product acceptance.

CI60 on feature head `a4126e87` now confirms the Linux compiler-rt resource fix:
the source-built producer passes relocation, sanitizer compile/link/run and
publishes its qualified toolchain. Five Linux consumers then fail **before
Forge configure**, because `/usr/bin/cmake` is absent. Their runtime gates remain
NOT_RUN. The reviewed CI-only follow-up installs ordinary distro `cmake` and
`ninja-build` before the four Linux consumer step sequences, records executable
and package identities, and fails closed on installation/evidence errors.
All original 71 metadata tests remain unchanged; eight new tests record RED
against the original workflow and GREEN with the candidate, **79/79** total.
LLVM pins/build mechanisms, absolute-path ABI checks, later pinned CMake3.31.6,
macOS steps, guards and runtime selections are unchanged. The workflow hash
normally produces a fresh recipe key; CI60 is not reused as that qualification.
Fresh real Linux execution is still required; CI61 does not include this follow-up. Evidence:
`wamr-ci-190-20261020-r11/ci60-review` and `linux-consumer-prereq-fix-r1`.
CI62 then fails before LLVM in the new negative metadata test: all eight
missing-tool subcases return the expected refusal but GNU Bash omits the expected
ERR diagnostic inside the mock function. The test-only correction explicitly
returns that failure to the caller, matching the other existing mocks. All 79
test methods, all 217 assertion calls, the original 71 tests and the production
workflow/guards are unchanged. Real RED is CI62; the old suite already passed
on macOS, so no local RED is claimed. Fresh Linux GREEN is required after this
one-line correction. Evidence: `wamr-ci-190-20261020-r11/ci62-review` and
`ci62-mock-return-fix-r1`.
The PR stays draft and must not be merged into dev under this authorization.
All mandatory product, Linux, bounded-profile, performance and final delivered
provenance requirements remain open until their own evidence closes them.

## Prepared Context Owner Follow-Up (2026-10-20)

Current Spine dev `230d4102` with the coherent CI58 merge tree `9cd98e54`
reproduces a native cleanup fault in the original
`cached_and_uncached_invocations_reset_memory_globals_and_run_start_once` case.
Its first, uncached half destroys the backend before resetting the allocator;
the external context remains alive. The context held a raw module-wrapper
reference and retained the engine module, but not the full preparation owner.
Its host-binding destructor therefore accessed an already-destroyed PMR
resource. The cached half had not yet executed. This is a real consumer
lifetime defect, not proof of a WAMR numeric failure or another provider copy.

The Forge-only correction pins the existing `prepared_module::impl` as the
first context member so that it is released after instance/binding teardown.
Five existing metadata operations move onto that same private owner; their
public methods delegate without signature changes. Matching checks preparation
identity and execution mode, avoiding wrapper-address reuse. No new owner
registry, parser, allocator, cache, WAMR patch, limit or cancellation policy is
introduced. Caller-owned host lifetime is unchanged.

The frozen candidate is published base `ae50b6f8` plus three runtime files and
one regression source, with WAMR still `f6fe6d5`. The normal fresh public build
proves four regressions fail against the old runtime (three native exits and
one failed identity assertion), then pass unchanged with the correction:
**4/4 cases, 37 assertions**. The complete canonical execution target passes
**54/54 cases**. Independent source and evidence review is scoped GO. Actual
dependency records resolve Boost 1.90 and libc++220108; the conflicting ambient
`CPATH=/usr/local/include`, which selected Boost 1.85 in the first fresh
attempt, is removed only from child-process environments. The failed attempt
and original product crash are retained, not relabelled.

Normal reinstall, matching SDK rebuild and all sixteen Spine guest contracts
pass with unchanged ABI goldens. The original focused reset/start case now
passes all twenty assertions for cache capacities zero and 32. This is not a
full product pass: the unfiltered 104-case chain suite reports eleven failures,
including obsolete diagnostic expectations and an unregistered DB object error
whose cause remains under investigation. The complete controller build fails
at two additional state-API test units with Clang source-location exhaustion;
its full suite and four optimizing gates remain blocked, not waived. All nine
original gates plus the full controller executable are still required. The
validation source is `9cd98e54` plus exactly the same runtime postimages, not
an unmodified published revision. Its VM/SDK sources match the PR base; the
newer CI59 synthetic merge `a92eb494` differs only in the seven published
CI/documentation files. Isolated Spine test adapters preserve assertions:
the removed parser container becomes `std::span`, the malformed-WASM oracle
uses the neutral parse error, and a private verifier-factory split reduces
one test module's compiler source-location load. The split still needs a real
full executable build and all original factory-using cases. No production
product source, live pin or public facade is changed by those test adapters.

Raw RED/GREEN results, actual header dependency records, preserved failing
binary, source freezes and independent reviews are under workspace evidence
`wamr-ci-190-20261020-r10/context-owner-fix-r1`. Earlier CI58/LLVM-free OFF
results retain their original revision scope; they do not qualify this fix.
Full product, Linux, bounded-profile, performance and final delivered-source
provenance gates remain open. PR190 stays draft and unmerged.

## Typed Admission Follow-Up (2026-10-20)

The full Spine run exposes a second source-confirmed migration regression.
The typed validation-only backend passes its registered host profile to
preparation, but only the execution purpose installs engine bindings and no
separate typed import resolution follows. Pre-migration Forge `3a6e388f`
resolved the host profile for `null_backend` too. Structurally valid WASM with
a wrong registered import signature can therefore pass this admission path.
This is not classified as harmless diagnostic drift. The exact cause of the
observed DB error still requires its own product reproduction.

The two-line correction calls existing `Registered::resolve(*_module)` after
module construction only for typed validation. It reuses the existing
`function_imports()` metadata and name/signature checker; execution binding
code is unchanged. Bare `nullptr_t` validation may keep unresolved imports,
but an explicitly empty registered profile rejects them. No new API, parser,
registry, engine patch, guest execution or host callback is introduced.

At local base `6ccbce61` plus the two-file candidate, the new five-case corpus
first records five missing-throw assertions against the old implementation
(two cases fail, three controls pass), then passes unchanged with the fix:
**5/5 cases, 22 assertions**. The full canonical prepared-validation target
passes **13/13 cases**, and the full execution target remains **54/54**.
An initial test-constructor compile error is preserved separately and is not
counted as behavioral RED. Independent review, the complete JIT/AOT registry,
normal installed delivery and post-fix product checks remain required.
Evidence: `wamr-ci-190-20261020-r10/typed-validation-host-profile-r1`.

## CI57 Follow-Up (2026-10-19)

[VM run57](https://github.com/SystasisIO/forge/actions/runs/39268640394) is
complete at PR head `2d4c3cab`, WAMR `cb1eb77`, and is not accepted as a full
pass. ARM interpreter passes 25/25. ARM JIT/AOT passes 81/82, including all
53 standalone AOT tests and the installed optimizing component with 5/5
nested tests. The sole failure is the original 8193-instance JIT case:
Mach-O compact-unwind cannot represent an address delta `0x50000c314`.

Both Intel interpreter configurations build successfully after the BLS fix
and pass 23/27 CTests. Unit/spec processes terminate with a numerical exception;
source diagnosis identifies division by WAMR's zero-byte coalesced memory page
width in the Forge instance ceiling calculation. Numerical exit alone is not
native-PC or floating-point evidence. The other two failures are independent
provenance checks: a missing FP-module map entry and a stale CMake harness hash.
All original donor test bodies, oracle rules, assertion and fixture hashes
remain unchanged when refreshing this metadata.

The Linux Stage B LLVM build/install completes. Stage C final-runtime CMake
compiler smoke fails with `cannot find -lc++`, before relocated ABI
qualification. The fix explicitly binds Stage A headers/libraries and pinned
LLD before `project()` compiler checks; final relocation/provider/ABI admission
remains unchanged. Two new regressions bring metadata tests to 57/57.
Dependent Linux runtime, JIT, sanitizer and package jobs do not execute because
the qualified toolchain artifact is absent; those are not VM runtime results.

The follow-up uses fork `f6fe6d5` and LLVM's existing Darwin DWARF EH plugin,
not another allocator or a reduced multi-instance test. The Forge zero-width
memory guard preserves ceiling zero; growable-zero controls still enforce the
configured page cap before guest initialization. API, limits, deterministic
numeric behavior, cancellation, W^X and cleanup requirements are unchanged.
Fresh remote qualification remains required.
Raw evidence and independent reviews are under `wamr-ci-190-20261019-r9`.
No merge or live product change is performed.

The first local combined follow-up is retained as 88/89: the native mapping
fixture lacked a genuine RO input once compact-unwind metadata was removed.
LLVM's Mach-O graph builder assigns RW to non-instruction input sections.
A test-only JITLink plugin now marks the fixture's unique constant section
Read through LLVM's graph API; mutable counter data remains RW. All original
`maps >= 3`, actual permission, 42/43 execution, fault, refund and recovery
assertions remain unchanged. This corrects the object-mapper test input rather
than weakening protection or introducing production mapping policy.

The final coherent Darwin ARM64 candidate passes **89/89 CTests in 126.11
seconds**, with zero failures, disabled or skipped tests: all 84 selected VM
gates (including both provenance checks), BLS, signer, normal dev install and
the two other canonical package consumers. The three installed VM consumers
pass, including 5/5 nested compiler/AOT/session checks. This includes the
unchanged full unit/spec suites, original 8193-instance case, all 53 standalone
AOT registrations, live guest-PC unwind, native-map denial/cleanup/recovery,
deterministic numeric/FP behavior and cancellation tests. Metadata tests are
57/57 and oracle transformation selftests 21/21; formatting, shell and diff
checks pass. R1 remains 88/89. This is local JIT-ON qualification, not fresh
Intel/Linux CI, physical LLVM-free OFF delivery or current-product acceptance.

The read-only product-gate snapshot identifies Spine `origin/dev` at
`230d41029d8e5f1a1986508577d7248f486acf33` and the published Turbine feature
branch `codex/compute-sdk-contract` at
`e8807e7f9e3d8e047c7224edde250158a0bf4317`. Turbine's `origin/dev` is only a
bootstrap snapshot, not its current implementation. That feature branch still
uses the retired `forge.execution.wasm` module/namespace/target in eight files;
the approved mechanical migration to `forge.vm.wasm.execution` is needed in an
isolated validation copy. It directly builds `vendor/forge`, so setting only
`Forge_DIR` does not switch its consumer source. This inspection is not a
current product build or process acceptance; no live copy or pin is changed.

The same-source BLS Debug retry has also recovered its missing artifact:
1/1 PASS in 1408.42 seconds, all 45 call markers, 16486 checks/zero failures,
and all 27 recorded source hashes match. This closes only that donor evidence
gap, not the Forge runtime, current-product or complete-release gates. The
initial upload failure and its original results below remain historical facts.

## Historical F6 Checkpoint

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

Remote runs48/49/50 establish real blockers, not runtime passes: exact 22.1.8 apt
packages and the Intel macOS bottle are unavailable; the official Linux X64
LLVM archive passes its hash check but has `RTTI=NO`, rejected by the current
integration gate. Run50 still fails both ARM profiles at the same executor
namespace lookup; direct leaf imports alone did not fix it.
Fresh publication link-map/provider and installed/product qualification also
remain separate gates. In particular, this JIT-ON matrix is not an LLVM-free
runtime or performance-equivalence claim, and does not transfer the F6 package
or product results to the publication branch. The migration remains unaccepted.

## CI50 Follow-Up (2026-10-19)

The ARM failure is reproduced locally with the actual Forge target graph and
CMake4.4.3: `profile.cppm:8` and `compiled_module.cppm:14` produce ambiguous
`forge` declarations, followed by the Clang ASTReader crash (frontend exit139).
The graph contains additional `@synth_0` BMI variants. A flat twelve-module
graph, the actual CI LLVM bottle and the full Boost1.92 headers did not reproduce
it independently. This is a generator/compiler graph interaction, not evidence
that the execution API or numeric profile needs to change.

A fresh control changes only CMake to the qualified 3.31.6 generator. Its
executor object builds without those variants; the complete JIT-OFF executor
then builds and passes **43/43 cases and 1,397 assertions**, with zero failures,
skips, aborts, timeouts or expected failures. Seven JIT cases are absent from
this compile-time OFF profile; the prior JIT-ON fifty-case result stays separate.
CI now selects checksum-pinned official CMake3.31.6 packages rather than changing
public namespaces, floating-point flags or test selection. CMake4.4 is not
qualified by this checkpoint. A fresh remote run is still required.

The publication's eight reviewed map-only relinks preserve every binary byte
and all 1,998 protected inputs. The physical audit covers seven execution
images, three compiler images and one byte-only generator. All sixty exact
WAMR marker definitions have a single correct role; no live cross-role WAMR
C provider was found. Forty WAMR TLS descriptor/backing checks resolve to
thread-local sections. SoftFloat's four TLS pairs are proven in its archive
object only: these ten final WAMR images do not extract that state. This result
does not establish runtime thread behavior or an LLVM-free package; LLVM22.1.8
is expected in this JIT-ON graph. Separate installed/product gates remain open.

Run51's Linux producer passes the signed, version/hash-pinned stage0 install
and disk admission (about 85 GiB free), then fails configuring libc++'s optional
clang-tidy test helpers because their installed Clang package requires another
LLVM dev package. The follow-up excludes upstream toolchain test/benchmark/doc
targets explicitly from both runtime-build graphs, matching the producer's
existing LLVM test exclusion. This does not remove Forge/WAMR/SDK tests or the
executed ABI, exception, destruction and sanitizer-runtime preflight. The new
metadata regression passes with the other checks (26/26); native source build
and remote runtime acceptance remain unqualified.

Linux toolchain work remains qualification in progress. The candidate makes
LLVM22.1.8/libc++220108 source provisioning an explicit, resource-bounded CI
producer, not a hidden package fallback. Its output must pass the existing
header, RTTI, C++ ABI and actual-library checks before any Forge lane consumes
it. Static harness checks do not qualify the native build, performance, package
or downstream products. Intel macOS provisioning is still a separate blocker.
No runtime source, engine pin, public API, license or live product pin changes
in this follow-up. Workspace receipts are under `wamr-ci-190-20261019-r6`.

## CI52 Follow-Up (2026-10-19)

The qualified-generator control now passes remotely: ARM64 interpreter job
117957074640 completes 25/25 root CTests in 4.51 seconds, with zero CTest
failures, disabled or skipped tests. Its raw counts include unit178, spec2428
and SDK executor43 cases; JIT-only cases are outside this OFF profile. The unit
output retains 45 assertionless-case warnings, so zero CTest skips is not a
claim that every historical donor case executes assertions. Independent review
matches the registry, XML, actual commands and checksum-verified artifact.

The actual tested checkout is merge `90e0c67e8c16c79adc47174c0c017a1f74087814`,
PR head `ddf9f44ef388b2adad1d88a2588c95e563fc6cb1` over dev
`7061cd35714535765811f3e338ae30bee3b0df3b`. The ancillary run metadata's base
field is not substituted for these verified checkout parents. CMake3.31.6 is
used; this validates the generator selection, not an upstream CMake4.4 repair.
The OFF upload does not contain complete physical LLVM-free linkage or any
installed-package acceptance. ARM JIT/compiler-AOT remains a separate gate.

The Linux producer passes StageA runtime installation and its stage0 DSO/client
ABI probe, then fails linking the StageB shared LLVM library. LLVM's real CXX
PIC probe fails: Clang22's Linux driver reports unused -stdlib under -Werror
when combined with -nostdinc++. A syntax-only Linux-target driver control
reproduces that error and passes after removing only -nostdinc++; it is not
native Linux runtime evidence. The correction retains explicit libc++ and
StageA headers, enables PIC, saves the real CMake cache, and requires both C
and CXX PIC support checks before the expensive native build. No cached
support result is fabricated, warning suppressed or linker refusal bypassed.

Intel's explicit candidate uses the checksum-pinned historical LLVM22.1.8
Sonoma bottle through normal Homebrew installation and relocation, with only
bottled dependencies. It is restricted to the native macOS15 x86 interpreter
OFF profile. Exact compiler/scanner paths, live libc++220108 headers and host
RTTI/EH remain required. LLVM library EH=OFF does not describe generated host
C++ exception support and does not qualify this bottle for JIT/compiler use.
The combined static/metadata suite passes 44/44; native Intel installation and
the corrected Linux bootstrap still require fresh remote qualification.

The fresh publication package attempt reaches the normal aggregate install
prerequisite but fails in unchanged crypto/pki x509 compilation: generic
Homebrew OpenSSL4 headers precede the selected OpenSSL3 include directory,
while the configured libraries remain OpenSSL3. No install prefix is produced;
installed interpreter, executor and optimizing/session tests are NOT_RUN.
No unrelated OpenSSL4 compatibility edits, partial prefix or source fallback
is used to turn that failure into a package pass. Workspace receipts are under
`wamr-ci-190-20261019-r7`; the complete migration remains unaccepted.

Run53 then exposes a provisioning-only assumption: the exact Intel bottle
contains no INSTALL_RECEIPT.json; Homebrew creates that receipt during pour.
The real 386,191,529-byte archive passes its pinned SHA check. Its metadata,
format and relocation are now left to Homebrew rather than a parallel tar
receipt validator. Current Homebrew also rejects formula/bottle path arguments
by default. The follow-up uses a disposable CI-local `forge/vm-ci` tap created
with `tap-new --no-git`, copies the unchanged SHA-verified upstream formula,
and invokes ordinary qualified-name info/deps/bottle installation. It creates
no remote repository and disables no path-security guard. Full formula name,
version, official Sonoma bottle SHA/origin and all installed compiler/header/
host ABI checks remain mandatory. All 44 metadata tests and a real-payload SHA
admission check pass; actual native Intel installation remains unqualified.

Run54 passes ordinary Intel bottle installation, then actual compiler startup
refuses a missing libz3.4.16.dylib: the dependency installer selected current
Z3 5.1 rather than the bottle's ABI4.16 dependency. A compatible Z3 4.16.0
Sonoma bottle and its upstream formula are checksum-pinned from the same
historical Homebrew ref. Read-only inspection of the actual x86_64 library
shows only system libc++/libSystem dependencies. The bounded correction pins
this one ABI leaf through the same managed tap and an explicit reviewed
one-line LLVM recipe dependency patch. Upstream input hashes, the recipe patch
and derived formula identity are retained. No engine source changes, fake ABI
symlink, loader override, dependency bypass or source-build fallback is allowed.
The existing native compiler/header/host/runtime gates still decide acceptance.
Independent scoped review and all 51 metadata tests pass; this does not qualify
the corrected native dependency closure without its fresh CI execution.

The completed ARM JIT/compiler-AOT job117957074632 builds successfully but
passes only 22/82 root CTests; 60 fail, with no disabled or skipped root tests.
Raw JIT output reports `Native JIT write protection is unavailable`. The AOT
subset fails 48/53 tests, mostly behind a generic preparation resource-admission
error; this does not prove memory-budget exhaustion or one common cause for
all failures. Its nested installed optimizing consumer actually runs five
tests: three pass and two runtime/session tests fail. These remote failures
are distinct from the local OpenSSL-blocked package attempt above.

The pinned WAMR already includes TargetConditionals.h directly; a missing-header
explanation was checked and rejected. On Darwin ARM, an ineffective pthread
write-protection facility must not be treated as safe MAP_JIT execution.
Before any mapping-policy correction, a separate bounded OS-only diagnostic
records the raw pthread capability and tests ordinary, non-MAP_JIT RW-to-RX
execution, denied RX writes and denied RW execution in isolated children.
It does not qualify Forge, WAMR, compiler output, memory admission or products.
No runtime guard, memory budget or mandatory test is weakened for this probe.

Run55 refuses to load the pinned Z3 formula because the current Homebrew
requires explicit trust of non-official formula names. The next bounded CI
correction uses Homebrew's documented per-formula trust only for the two
checksum-verified managed formulas after source/patch/derived checks, before
metadata loading. It does not trust the entire tap, disable a security guard
or grant trust on the user's machine. Native Intel acceptance still requires
the unchanged real compiler/header/ABI and runtime checks. All 53 metadata
tests pass, including trust ordering and fatal unsupported-command controls.
The local OS-only diagnostic reports raw pthread support1, RX execution42,
and SIGBUS for both forbidden RX writes and RW execution; this is not evidence
of the remote runner's capability.

## CI56 And Native Protection Follow-Up

The [remote OS-only diagnostic](https://github.com/SystasisIO/forge/actions/runs/39240391860)
completes successfully on macOS15.7.9 ARM64. Raw pthread JIT protection support
is zero. Ordinary non-MAP_JIT RW/NX-to-RX code returns42; RX writes and RW
execution are independently denied with SIGBUS. Mapping/protection/cleanup
errors and compile/run exits are zero. The artifact/source/binary hashes are
verified; no downloaded binary is executed locally. This is an OS proof, not
runtime, package or product acceptance.

Fork `cb1eb77` and the matching Forge caller now use an immutable committed
native-code protection policy. Supported MAP_JIT keeps its real per-thread
protection. Support=0 uses ordinary writable/nonexecuting preparation followed
by checked, sealed RX; it does not turn an unsupported MAP_JIT mapping into
unchecked execution. AOT and ORC policy publication follows complete loader/
allocation finalization. Unknown/uncommitted native code is rejected before
guest initialization or calls. Existing owner records/allocators are reused.

The broader local R1 source suite passes80/81, exposing a separate invocation
cancellation-delivery race. The fix uses the existing absolute watchdog for
the runtime-owned stop-forwarding callback, with a saturating deadline and
unchanged overlap/error precedence. Generic duration-watchdog callback delivery
is not changed. Ten fresh invocation repetitions pass; the added tests require
the C++ continuation after synchronous cancellation to remain zero, across
interpreter/JIT initialization, calls, maximum-duration budgets and reuse.

The coherent cb1 compiler/runtime R3 source matrix passes81/81 in99.45s,
including all53 AOT tests, with no disabled/skipped CTests. The standalone
production-private real-loader graph separately passes1/1 using fresh raw AOT
from the same compiler identity. It checks shared accounting, one-byte budget
shortfall, actual text/data protections, allocation/mapping/protection/cancel/
unmap faults, cross-thread unload and recovery after repeated cleanup failures.
These results do not substitute for the installed-package gate excluded from
this local source selection. Its new normal aggregate dev-component build and
install complete after selecting the actual Boost keg dependency configs;
the real x509 compile command no longer places broad Homebrew OpenSSL4 headers
before the selected OpenSSL3 headers/libraries. No compatibility shim, source
fallback or partial install is used. Fresh canonical installed consumers then
pass3/3 in95.18s: optimizing69.34s, SDK execution7.83s and relocated interpreter/
JIT/profile17.99s. The optimizing consumer's nested real compiler/runtime/
session suite passes5/5 in10.23s, with no skipped/disabled tests. Raw logs/XML
are retained in workspace `wamr-ci-190-20261019-r8`. This is local JIT-ON package
evidence, not a physically LLVM-free OFF delivery, remote or product pass.

The proposed REL-to-XIP header-flip controls are withdrawn: they did not emit
valid indirect XIP code and failed before the claimed canonical check. The
R2 evidence remains77/81, not relabelled a pass. Independent review confirms
that only those new invalid controls were removed; all original assertions,
registrations, quotas and error types remain. The small canonical admission
guard stays defense in depth; a reachable borrowed-XIP boundary defect is not
claimed. Real private-policy refusal and mapping-fault controls remain.

In [VM run56](https://github.com/SystasisIO/forge/actions/runs/39240391559),
ARM interpreter OFF passes25/25, independently reviewed against the source,
registry, XML and raw artifact. Actual checkout is merge `e8947533` from dev
`67ca70c0` and PR head `1b92ac38`; the runtime pin is69c107c, not cb1.
The Intel bottle/ABI installation now passes; both Intel builds then fail in
the donor BLS library's ELF assembly selection on Mach-O. The authorized
SystasisIO BLS fork is tracked in [draft PR1](https://github.com/SystasisIO/bls12-381/pull/1), with native donor evidence kept
separate from Forge qualification. ARM JIT/AOT subsequently completes22/82:
60 root CTests fail, no root skips/disabled tests. The actual JIT diagnostic
again refuses unavailable native write protection. Generic AOT admission
errors do not by themselves prove budget exhaustion or one common cause.
This is old69c evidence, not a remote result for the new cb1 policy.

Run56's Linux producer fails because GitHub reports that the hosted runner
lost communication. Logs return BlobNotFound and its final evidence upload
never runs. Downstream Linux gates therefore have no qualified toolchain;
these failures do not establish a VM defect or a source-bootstrap pass.
The preserved run53 evidence separately proves a missing StageB MODULE LLD
flag and mismatched PATH-selected CMake/Ninja identities. The follow-up pins
MODULE like EXE/SHARED and invokes the actual hashed /usr/bin CMake/Ninja in
all source/qualification graphs. All55 metadata tests pass; fresh native
bootstrap and relocation/ABI checks remain mandatory.

The user authorizes cancelling only obsolete runs53-55:53 had already failed,
54/55 are cancelled. Run56 and the OS diagnostic are not cancelled. No merge,
live Spine/Turbine edit, license change or mandatory-gate waiver is performed.

### Intel BLS Dependency

The authorized BLS fork is now pinned experimentally to
`c0d27c3bace8dc24cfb16fae34de661df196e538`. Its production delta selects the
existing portable C++ arithmetic on Darwin x86 instead of ELF assembly. The
portable path also had an existing carry/borrow overflow bug: boundary and
fixed-seed checks reproduce 12 mismatches before the two minimal helper fixes
and zero afterwards. No cryptosystem, assembly implementation, license or API
is replaced. This is not a constant-time or performance-equivalence claim.

Native donor Release suites pass on Linux normal/no-BMI2, Darwin x86 and ARM.
The full original Debug suite twice exceeds the initial 900-second CI budget;
diagnostic-only progress preserves all 45 calls, order, assertions and inputs.
A reviewed, one-shot Intel Debug CI budget of 1800 seconds then completes the
full suite: 1/1 PASS in 1403.54 seconds with strict XML verification. Other
entries remain at 900 seconds; VM/product deadlines and limits never change.

That job subsequently fails only uploading its evidence after five network
timeouts. Its raw log and checkout identity are retained, but the missing
Debug artifact keeps the complete donor evidence gate NOT_ACCEPTED. Scoped
review permits experimental pinning, not release/merge approval. Only the
failed job is retried at the same source; all fresh Forge native/platform and
artifact gates remain mandatory. See [native run3](https://github.com/SystasisIO/bls12-381/actions/runs/39261942138).

With this pin, the normal Forge BLS/signer/dev-install build and coherent VM
test rebuild pass. The final combined selection passes 86/86 in 201.22 seconds,
with zero failures, disabled or skipped tests: all 82 selected VM gates plus
BLS, signer and the two other canonical package consumers. BLS executes 24
Boost cases and signer 37; nested compiler/AOT/session tests pass 5/5. The
same strict registry checker verifies all 82 VM commands/helpers/maps before
execution. Workspace evidence is `wamr-ci-190-20261019-r8/bls-forge-integration-r1`;
this local result does not reclassify missing remote Debug artifacts or qualify
remote Intel/Linux, physical LLVM-free OFF delivery or downstream processes.

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
