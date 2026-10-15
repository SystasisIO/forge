# CJOSE backend qualification (F0-B)

CI-only standalone C tests; no Forge catalog build, future attestation API,
plugin migration, JWT claims, Raw codec, Chain API or provider lifecycle.
The strict profile in `profile.c` is explicitly a **qualification fixture**,
not production enforcement and not a replacement JOSE/JSON parser.

## Sources and Execution

`pins.json` fixes unmodified OpenIDC CJOSE 0.8.1 at
`6fdb5fe75a86acb9cd34ca2eb13d9e090a6df588` and Jansson 2.14.1 at
`ed5cae4ed0621ef409510f94270c9f8f263736d0`. The candidate is the exact
`vendor/cjose` gitlink of the checked-out Forge commit, fetched from
SystasisIO/cjose and checked for upstream-base ancestry. Never resolve a
candidate branch or mutable HEAD. Historical 0.6.2.8 at
`e787ace6a203d483038a793fd58f958a2f9b48c1` is diagnostic, not the acceptance base.

Push the qualification branch or dispatch the CJOSE Qualification workflow.
Only that branch runs. The driver refuses non-GitHub-CI execution. No local
compilation or Docker is required. Native Ubuntu/macOS builds use CMake,
`CJOSE_BUILD_SHARED/STATIC/TESTS=ON`, `CJOSE_MACOS_DYLIB=ON`. Historical
autotools runs in its generated source clone to avoid its known out-of-tree
test-header lookup issue; no source patch is applied.

macOS uses the version-gated Homebrew pair `llvm@22` / `lld@22`, both exactly
22.1.8 as fixed in `pins.json`, instead of mutable unversioned LLVM plus the
default system linker. All owned CMake executable/shared/module links and the
historical autotools lane select the absolute `ld64.lld` path explicitly.
Preflight checks exact compiler/linker versions, records binary SHA-256 and SDK
version, and checks clang's `-###` plan selects that linker without executing a
build. `macos-toolchain.json` PASS proves selection only, not sanitizer/runtime
qualification; every existing macOS gate must still execute. Missing/mismatched
tools fail, with no fallback, skip or sanitizer disable. Bottle/source hashes are
not pinned by this version check; actual installed compiler/linker hashes are
recorded for the run.

This change follows 7f1043eb evidence: clang 23.1.0's sanitizer compiler-check
failed at link time with `ld: invalid r_symbolnum=1` before CJOSE execution.
That evidence does not prove the exact upstream defect or establish that the
replacement pair works; only the next CI can do that. Sources:
[LLVM Mach-O linker selection](https://lld.llvm.org/MachO/index.html#using-lld),
[Homebrew LLVM 22](https://formulae.brew.sh/formula/llvm@22),
[Homebrew LLD 22](https://formulae.brew.sh/formula/lld@22).

## Fixed Fixture Bounds

These limits are defined before CI execution, not inferred from observations:

| Item | Limit | Reason |
| --- | ---: | --- |
| Decoded protected header | 1024 bytes | Three bounded fields plus whitespace; no certificate/key/URL documents |
| Binary payload | 65536 bytes | Exercises binary/NUL and a substantial bounded message, not whole-file storage |
| Compact token | 100000 bytes | Maximum valid token is 1366 + 1 + 87382 + 1 + 86 = 88836 bytes |
| `kid` | 1..128 ASCII bytes | Opaque bounded identifier, not a URL or path; letters/digits/`-_.` only |
| JOSE signature | Exactly 64 bytes | ES256 unsigned fixed-width `r || s`, no DER or recovery byte |
| Probe protocol line | Less than 400000 bytes | Hex-framed test transport comfortably holds the maximum token |
| Probe response deadline | 15 seconds | Hard per-response deadline; outer qualification phase 360 seconds |
| Measured probe peak RSS | At most 1024 MiB | Evaluated from OS child resource counters, not a virtual-address limit |
| Allocator positions | At most 4096 per operation | Measured successful-path allocation count; exceeding cap fails |
| Fuzz | 20000 runs OR 60 seconds | 100001-byte input cap, 2-second input timeout, 1024-MiB RSS cap |

The base64url maximum is `4 * floor(n / 3) + (n % 3 ? n % 3 + 1 : 0)`.
Empty payload is supported by backend JWS but rejected by this staged profile
to avoid detached-payload ambiguity. Profile header literals are exactly
`alg=ES256`, `typ=application/ci-jws+octets`, bounded `kid`, with no other fields.
These are test literals, not future product wire contracts.

## Gates

| Gate | Implemented execution |
| --- | --- |
| Q1 | Native unmodified 0.8.1 suite, native candidate suite, and the exact original 0.8.1 test source files compiled against candidate objects. Corpus hashes, Check counts/test IDs/failure IDs and exit codes retained. Missing Check executable or zero tests is failure, never skip/PASS. Historical in-source suite reported independently. |
| Q2 | RFC 7515 A.3 exact signing input, SHA-256 and known signature; external prehashed ECDSA signature completed by CJOSE and verified by pinned JWCrypto; JWCrypto-signed binary payload verified by CJOSE. No JWT claims processing. |
| Q3 | Mutate/free caller header, payload and public key; immutable input/digest before/after complete; wrong key/input/algorithm/curve; zero/short/long/DER/recovery-byte/corrupt/double-hashed signatures; failure retry, successful single-use, independent release order; eight distinct states completed on native pthreads. |
| Q4 | Public CJOSE base64/import/verify and public Jansson duplicate-rejecting JSON/UTF-8 decode. Canonical base64url re-encoding, exact header allowlist, parsed-object/payload differential checks. Signed malformed/duplicate/NUL/unknown/critical/URL/key-injection/type cases plus segment and encoding errors. No key-fetch implementation or network client. |
| Q5 | Exact minima/maxima/overflow cases; malformed nesting; fail-once allocator sweeps for prepare/complete, recovery and no partial credential, ASan/UBSan backend+fixture, bounded libFuzzer decode/profile/prepare/complete. Measured per-case elapsed time, per-operation RPC latency and child peak RSS; fuzz run/coverage/RSS report. |
| Q8a | Native install and genuine public C consumers linked separately to shared/static installed targets on both operating systems. Feature macro version 1 and all five functions used. Real dependency-tree resolution requires one selected CJOSE shared library (or none for static), one OpenSSL libcrypto, one pinned Jansson. Forge FindCJOSE configure accepts candidate but rejects unmodified 0.8.1 for missing extension, not a missing package. |

### Historical Binary ABI Gate

The additional required `legacy-abi` phase is **binary compatibility**, not a
recompiled-source substitute. After `make install` from the generated historical
clone, `legacy_abi/CMakeLists.txt` builds `legacyconsumer.c` once, exclusively
against installed 0.6.2.8 public headers and shared library. A preprocessor guard
rejects modern macro headers. The consumer references and checks all 33 historical
`CJOSE_HDR_*` external data symbols and exercises public header APIs and the RFC
7515 A.3 public-key import/JWS import/verify/export path.

The driver runs the unchanged binary first with 0.6.2.8, then with patched 0.8.1.
Only each subprocess receives `LD_LIBRARY_PATH` or `DYLD_LIBRARY_PATH`; inherited
loader overrides are removed, and no runner/global environment is changed. Native
`dladdr(cjose_jws_import)` and each legacy constant's address must resolve to the
exact selected installed library after realpath normalization. Version checks,
binary SHA-256 before/after both runs, selected library hash and fixed-schema
status are retained in `legacy-abi-result.json`. Missing install, failed loader
substitution, missing symbols or absent proof is FAIL, never SKIP. Platform loader
version/install-name incompatibility is a real failing result, not patched around
by relinking, copying or renaming libraries. This is targeted ABI coverage, not
an exhaustive compatibility proof for every old CJOSE function or platform.
The historical public `jwk.h` includes OpenSSL headers; this consumer takes
include requirements from the selected `OpenSSL::Crypto` target without adding
a second direct crypto link or implementing cryptographic operations itself.

### Package Feature Controls

Each case calls `find_package(CJOSE QUIET MODULE)` exactly once, with no native
prelookup. Candidate and unmodified upstream must both configure with exit 0 and
`QUALIFICATION_FEATURE_RESULT_PASS`: native package discovery and the separate
`FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES` compile/link control must succeed. The
extension probe must be defined; it and `CJOSE_FOUND` must be true only for the
candidate. An unrelated compiler/package/dependency error cannot pass as a
negative extension result.

Only the candidate also exercises repeated native config lookup, quiet rejection
of an unknown required component, successful recovery to the static component,
and exact `FindJansson` 2.14.1 discovery. A separate candidate configure disables
the lowercase native Jansson package with
`CMAKE_DISABLE_FIND_PACKAGE_jansson=TRUE`; fallback discovery must find the same
pinned dependency include/library prefix and exact header release version. The
original upstream source/config remains unmodified and is never used for these
candidate-only package recovery tests. All three configure results are required.
Both candidate discovery modes also require a wrong `2.14.0 EXACT` request to
return false, followed by successful exact 2.14.1 recovery. During native-config
version checks, `CMAKE_INCLUDE_PATH` contains an unrelated 9.9.9 header decoy;
the selected target's real header must remain authoritative. The decoy is never
compiled and no alternate library is built.

The independently pinned Python closure is in `requirements.txt`. Private
ephemeral keys are generated/held only inside Python; C receives a strictly
allowlisted public JWK, never a private field or a private-key file. The C
probe does not generate a key or call `cjose_jws_sign`. Unmodified upstream
Q1 tests necessarily retain their own upstream private test fixtures; the
public-only rule applies to the new external-signing qualification probe.
Extension-unit-suite coverage owns the backend's private-key-rejection test.

## Evidence and Remaining Gaps

All gates are **NOT_RUN until real CI execution**. Source/syntax checks do not
qualify this backend. Final `evidence/summary.json` fails for missing/failed
required artifacts; failures in the original acceptance baseline are retained
and not waived. Q1 comparison separately identifies baseline and new failures.
Historical 0.6.2.8 failures are preserved as diagnostics, not reclassified as
candidate regressions. Q7 existing Forge/OAuth regressions are coordinator-owned.

Raw subprocess logs, probe stderr and operation token responses remain under
runner-local work/raw or case directories, never uploaded or printed. Uploaded
evidence contains fixed commands with normalized paths, exact SHAs, toolchain
and Python package manifest, test IDs/status/exit codes, numeric metrics and
bounded compiler/configure diagnostics. The separately labelled synthetic fuzz
corpus is retained by content hash for reproduction; it contains only public
RFC vectors and generated, non-production fixture inputs, never private keys
or production credentials. No environment dump or runtime token log is selected.
Do not upload entire work directories.

`safe-build-diagnostics.json` also selects feature-detection CMake errors and
bounded multiline error context. `feature-trycompile-diagnostics.json` contains
only selected feature/compiler-ABI/compiler-works try-compile errors and
allowlisted compiler invocation flags from owned `CMakeConfigureLog.yaml` files,
including sanitizer compiler checks. Source buffers, full compile commands,
environment values and runtime logs are excluded; sensitive diagnostic lines
are omitted. Pure-Python selector regressions run with
`python3 -B tests/cjose_qualification/test_diagnostics.py`, without a compiler.

Ubuntu LeakSanitizer is enabled; macOS LeakSanitizer is unsupported and marked
explicitly. Jansson/OpenSSL are not ASan/UBSan-instrumented; the candidate CJOSE
and public fixtures are. Allocation hooks cover CJOSE/Jansson/OpenSSL in a warm
process, not every possible cold OpenSSL initialization path, allocator/race
schedule or exhaustive allocation-failure combination. There is no ThreadSanitizer
claim. A bounded fuzz run is not an exhaustive security proof. This fixture does
not qualify any production codec, custody provider, cancellation or plugin API.

References: [JWS RFC 7515](https://www.rfc-editor.org/rfc/rfc7515.html),
[ES256 RFC 7518](https://www.rfc-editor.org/rfc/rfc7518.html#section-3.4),
[JWCrypto 1.5.6 public API](https://jwcrypto.readthedocs.io/en/v1.5.6/jws.html).
RFC A.3 fixture data is public test-vector material from RFC 7515, copyright
2015 IETF Trust and the identified authors, used under its stated Trust Legal
Provisions. No RFC private key is included.
