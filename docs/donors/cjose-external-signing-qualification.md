# CJOSE External Signing Qualification (F0-B)

Status: F0-B complete, with all required CI gates passed and two independent
Sol 6.1 xHigh reviews returning `CLEAN` on 2026-10-15.
This report does not approve integration into `dev`, a new signing service or
the later Forge attestation API. No upstream issue or pull request is published.

## Scope And Source

| Item | Revision / ownership |
|---|---|
| Forge base | `8975f234dabf2cbfb91d766bf32194f3d8832a5b` |
| Forge branch | `cjose-external-signing-qualification-v1` |
| Tested Forge source | `f106050daf876ce404abdf9ba1234db737ba2bec` |
| Historical CJOSE | OpenIDC `0.6.2.8`, `e787ace6a203d483038a793fd58f958a2f9b48c1` |
| Extension base | OpenIDC `0.8.1`, `6fdb5fe75a86acb9cd34ca2eb13d9e090a6df588` |
| Maintained fork | [SystasisIO/cjose](https://github.com/SystasisIO/cjose), `external-signing-0.8-v1` |
| Candidate | `84da8b54feb4e46ea1ddae6930f821e1510471b7`; exact immutable `vendor/cjose` gitlink |
| Jansson | `2.14.1`, `ed5cae4ed0621ef409510f94270c9f8f263736d0` |

Fresh upstream inspection found no public two-phase external-signing API in
0.8.1. The extension is a feature request, not itself a vulnerability fix.
Use the maintenance release rather than independently backporting its fixes.
Some changelog entries were already present in the historical OpenIDC pin;
the report does not claim all intervening changes are new fixes for that pin.

Existing JWS functions and shared-library ABI remain available. New upstream
validation rejects several malformed JWE/JWK inputs; the default RSA1_5
encryption option is disabled. Forge's token algorithm allowlist is unchanged.
The extension is limited to ES256 (P-256 / SHA-256), not the other algorithms
added by upstream. See the immutable [release notes](https://github.com/OpenIDC/cjose/blob/6fdb5fe75a86acb9cd34ca2eb13d9e090a6df588/CHANGELOG.md).

## Public C Boundary

The added opaque `cjose_jws_prepared_t` owns a copied public-only verification
key, protected header, payload and exact signing input. It does not retain
caller's mutable buffers or a private key. `cjose_jws_prepare` performs no
signature and makes no callback. Input and digest accessors expose borrowed
read-only views whose lifetime ends at `cjose_jws_prepared_release`.

An external provider signs SHA-256 of that exact input. `cjose_jws_complete`
accepts the 64-byte JOSE `r || s` signature, verifies it against the frozen
public key and returns a regular `cjose_jws_t`. It does not reserialize the
input or accept a different algorithm/key. Invalid completion is retryable;
successful completion is single-use. The returned JWS and prepared state
have independent lifetimes. Calls mutating one state must be serialized;
independent states share no per-request mutable state.

The backend remains synchronous C. Awaiting a custody provider between
prepare and complete belongs to a future caller, not CJOSE. There is no
fake-signature callback, export of private keys, embedded HSM client or
second JOSE implementation. Existing encoding, digest, verification and
export functions are reused internally. Consumers use installed public
headers only, never `src/include/*_int.h`.

## Packaging

The source pin is a submodule, not an untracked vendor modification.
`cmake/ForgeCJOSE.cmake` embeds upstream's CMake static target and default
`dev` installation component, without reconstructing its source list.
OAuth uses the same target. Jansson is resolved once through the upstream
dependency target; CJOSE and Forge share `OpenSSL::Crypto`.

The backend itself has no OAuth/libcurl/PCRE2 dependency. Installed Forge
consumers require the extended API by header feature level and linked public
symbols, not merely `0.8.1`. Host-only optional OAuth configuration enables
this backend; the Contract SDK profile does not pull it into guest code.
Build-tree lookup uses Forge's existing `_forge_require_package` mechanism,
preserving the outer lookup's optional/QUIET behavior. Missing dependencies
must yield `Forge_FOUND=FALSE` for an optional lookup rather than an
unconditional configure failure; Q7 includes explicit negative consumers.

## Qualification Contract

The fixture profile is deliberately not a production Forge codec or an OAuth
token. It uses `typ=application/ci-jws+octets`, `alg=ES256` and a bounded `kid`.
The signing payload is arbitrary binary data, not JWT claims or future Forge
Raw records. Accepted limits, fixed before execution, are decoded protected
header 1,024 bytes, payload 65,536 bytes, complete token 100,000 bytes, key ID
128 ASCII characters and signature exactly 64 bytes. They bound parsing and
allocation for qualification; F1 must publish its own versioned wire profile.

The fixture enforces compact-segment/size/alphabet constraints and uses public
CJOSE base64/verification and Jansson JSON validation. It rejects duplicates,
unknown headers and parser disagreements. Passing fixture rejection does not
claim that unrestricted CJOSE itself applies all product profile rules.

| Gate | Evidence required | Current result |
|---|---|---|
| Q1 | Unmodified 0.8.1 tests and same suite plus extension on candidate; historical baseline separate | PASS, both operating systems |
| Q2 | RFC signing-input/vector and bidirectional independent JWS interoperability | PASS, both operating systems |
| Q3 | Public-only key, frozen input, substitution, encoding/length, completion/lifetime and independent states | PASS, both operating systems |
| Q4 | Malformed JWS/header corpus, explicit profile rejection, no parser differential | PASS, both operating systems |
| Q5 | Bounds, overflow, allocation failures, bounded fuzz, ASan/UBSan, measured resources | PASS within stated instrumentation limits |
| Q7 | Existing Forge OAuth verifier and patched liboauth2 regressions against original and candidate dependency closure | PASS, 140/140 in each lane, identical case IDs |
| Q8a | Linux/macOS shared/static install, public consumer, old C ABI and extension feature detection | PASS; Forge installed/build-tree consumers and optional-dependency rejection also passed |

CI must record exact Forge/CJOSE SHAs, compiler/dependency versions, commands,
exit codes, test counts and artifacts. Missing tests, baseline failures or
skipped sanitizer capabilities are not PASS. Newly produced ECDSA signatures
need not be byte-identical; known vectors and signing input must match.

Credentials in fixtures are disposable. Evidence must exclude private keys,
token bodies and arbitrary provider responses. This task creates no local
Docker environment or large local build; CI workspaces are ephemeral.

Q6 (async custody), Q8b (new Forge/guest packages), Q9a (Raw document boundary)
and Q9b (signer profiles) remain NOT_RUN and outside F0-B. No GateOS/spinekd
pin, plugin topology, public Forge API or production permission format changes
in this branch. Integration is a separate decision after code review is CLEAN.

## Executed Evidence

[Backend CI 38745474616](https://github.com/SystasisIO/forge/actions/runs/38745474616)
passed on Forge `f106050daf876ce404abdf9ba1234db737ba2bec` with the CJOSE
candidate above. Both independent reviewers inspected the actual artifacts,
source and corpus hashes, not only GitHub's green conclusion.

| Evidence | Linux | macOS |
|---|---|---|
| Upstream / candidate / original suite against candidate / sanitizer suite | 122 / 127 / 122 / 127 PASS | 122 / 127 / 122 / 127 PASS |
| Public C consumers, shared/static and ordinary/instrumented | 4 x 74 PASS | 4 x 74 PASS |
| Injected allocation-failure positions, prepare / complete | 244 / 30 | 188 / 30 |
| Bounded fuzz runs / peak resident memory | 20,000 / 475 MiB | 20,000 / 488 MiB |
| Unchanged historical binary, old and candidate shared library | PASS, actual loaded library verified | PASS, actual loaded library verified |
| Native/fallback package discovery and negative extension control | PASS | PASS |

ASan/UBSan instrument CJOSE and the fixture, not the separately built
Jansson/OpenSSL dependencies. macOS LeakSanitizer is unsupported and not
claimed as tested. macOS uses verified LLVM/LLD 22.1.8; the sanitizer build
keeps SDK-deprecated functions in upstream tests as visible warnings only for
`check_cjose`. Production objects, assertions and instrumentation are unchanged.
The [fixture README](../../tests/cjose_qualification/README.md) records these
qualification policies and the observed setup failures.

The original 122 test IDs and hashes of ten original test files are preserved.
Five upstream-style tests exercise the new API; additional public-consumer
cases supply interoperability, failure injection and bounded malformed inputs.
These results do not qualify a future network signer or production permission
profile.

[Q7 CI 38745474876](https://github.com/SystasisIO/forge/actions/runs/38745474876)
passed against the same final Forge source and the original `8975f234` baseline
in one runner environment. Both lanes use the unchanged liboauth2 pin
`ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5`. Candidate links the sole vendored
static CJOSE; baseline uses isolated historical shared CJOSE. Actual linkage
checks passed, not just successful discovery of headers.

| Q7 evidence | Baseline | Candidate |
|---|---|---|
| liboauth2 verifier | 18/18 PASS | 18/18 PASS |
| Token verifier | 8/8 PASS | 8/8 PASS |
| Trusted subject / transport boundary | 21/21 PASS | 21/21 PASS |
| HTTP auth | 7/7 PASS | 7/7 PASS |
| Patched liboauth2 regression cases | 86/86 PASS | 86/86 PASS |
| Default dev install and consumer | PASS | PASS |
| Build-tree consumer | NOT_REQUIRED | PASS |
| Missing dependency, optional build-tree / installed discovery | NOT_REQUIRED | PASS / PASS |
| Removed, skipped or regressed cases | None | None |

The negative consumers disable Jansson discovery and require an optional
`find_package(Forge CONFIG QUIET COMPONENTS auth_liboauth2)` to finish normally
with `Forge_FOUND=FALSE` and dependency diagnostics. A configure error, absent
marker or skipped negative consumer fails the gate. The tests exercise the
real exported packages, not synthetic archives or placeholder targets.

Q7 uses Ubuntu 26.04, Clang 22.1.2, libstdc++, Ninja 1.13.2, CMake 4.4.4,
Boost 1.92.0, OpenSSL 3.5.9 and Jansson 2.14.1. Scheduling is `-j4` with a
two-job compile pool and one-job link pool to bound CI memory. Standalone
backend qualification also covers Linux Clang 18/OpenSSL 3.0.13 and macOS
LLVM/LLD 22.1.8/OpenSSL 3.6.4.

Each Q7 lane deleted its own successful generated build/install/consumer
trees after collecting evidence: candidate 9,826,124 KiB across six directories,
baseline 9,695,036 KiB across three. Both cleanup reports are `COMPLETE`, with
zero remaining bytes in those paths and recorded before/after free space.
No local compile or Docker run was used. Downloaded evidence is not a build
tree and can be removed after review; source worktrees and commits remain.

The [durable result summary](cjose-external-signing-results.json) retains
source pins, gate counts and SHA-256 digests of the final CI reports. GitHub
artifacts include case IDs, corpus hashes and bounded diagnostics; they have
finite retention. Reproduce them from the tested source, not a moving branch.
The Q7 collector intentionally keeps `f0_b_closed=false`: it only judges Q7,
not the independent backend results or reviewers' overall acceptance.

Additional compatibility matrices are explicitly NOT_RUN: OAuth against
unmodified 0.8.1 as a third intermediate state, and cross-version persisted
HS256 / dir-A256GCM data in both directions. Existing auth regressions pass,
but this is not a claim of those broader persisted-data migrations. They are
not required by the scoped Q7 acceptance contract; qualify them before any
integration that relies on these persisted representations.

## Final Review And Delivery

Both independent reviewers returned `CLEAN` for the complete scoped F0-B
on the exact tested Forge/CJOSE pair, covering Q1-Q5/Q7/Q8a, source ownership,
security, packaging and actual CI artifacts. The optional-dependency discovery
finding is closed by the source fix and both real negative consumers. Both
reviewers also returned `DOCS_CLEAN` for this report and the result summary.
No required F0-B gate or actionable finding remains open.

The subsequent documentation-only commit records this acceptance without
claiming a fresh runtime test of that commit. The tested implementation remains
`f106050daf876ce404abdf9ba1234db737ba2bec`; no source or CI logic changes follow
it in the delivery. Both feature branches are published for inspection. There
is no `dev` merge, release/tag, downstream pin update or F1/F2 implementation.
Integration requires the next explicit decision.

## Upstream Handoff

Qualification also reproduced two independent CMake packaging defects in
unmodified 0.8.1. Keep their maintenance fix separate from the signing feature:

| Defect | Reproducer / required regression |
|---|---|
| Repeated `find_package(cjose)` recreates the existing `cjose::cjose` alias | Find the installed candidate package twice in one configure; both calls must succeed and preserve the selected targets |
| `FindJansson` treats the Jansson CMake package's ABI version as its release | Jansson source pin above installs package version `4.14.0`, but its public header declares release `2.14.1`; discovery must inspect the selected dependency, not fail an exact release requirement or find a different installation |

Forge's feature detection separately compiles and links the selected dependency
headers and original CJOSE API before probing the extension. The unmodified
0.8.1 negative test must first pass that control and then reject the missing
extension. An unrelated configure/link failure cannot satisfy this test.

Proposed feature: expose owning preparation and verified completion for
external ES256 signing while preserving the original signing API. Include
the minimal CJOSE commit diff, a public-only key example, immutable-input and
failure-cleanup tests, CI platforms and exact upstream base. No network or
async framework is required in CJOSE. Submit only after explicit authorization.

If upstream accepts an equivalent public interface, qualify it against the
same fixtures before dropping the maintained patch. Do not silently replace
the exact pin or accept an unextended package with the same version string.
