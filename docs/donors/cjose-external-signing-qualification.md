# CJOSE External Signing Qualification (F0-B)

Status: backend qualified with two independent `BACKEND_CLEAN` reviews;
Forge OAuth/install regression gate Q7 is still pending. F0-B is not closed.
This report does not approve integration into `dev`, a new signing service or
the later Forge attestation API. No upstream issue or pull request is published.

## Scope And Source

| Item | Revision / ownership |
|---|---|
| Forge base | `8975f234dabf2cbfb91d766bf32194f3d8832a5b` |
| Forge branch | `cjose-external-signing-qualification-v1` |
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
| Q7 | Existing Forge OAuth verifier and patched liboauth2 regressions against original and candidate dependency closure | PENDING; not covered by standalone backend results |
| Q8a | Linux/macOS shared/static install, public consumer, old C ABI and extension feature detection | PASS for standalone CJOSE; Forge package acceptance also requires Q7 |

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

[Backend CI 38726696956](https://github.com/SystasisIO/forge/actions/runs/38726696956)
passed on Forge `ac580f71956e24bc862c29316b970eced57734a9` with the CJOSE
candidate above. Both independent reviewers inspected the actual artifacts,
source and corpus hashes, not only GitHub's green conclusion.

| Evidence | Linux | macOS |
|---|---|---|
| Upstream / candidate / original suite against candidate / sanitizer suite | 122 / 127 / 122 / 127 PASS | 122 / 127 / 122 / 127 PASS |
| Public C consumers, shared/static and ordinary/instrumented | 4 x 74 PASS | 4 x 74 PASS |
| Injected allocation-failure positions, prepare / complete | 244 / 30 | 188 / 30 |
| Bounded fuzz runs / peak resident memory | 20,000 / 497 MiB | 20,000 / 484 MiB |
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
profile. Q7 and its independent final review remain mandatory before F0-B closes.

[Q7 run 38723943582](https://github.com/SystasisIO/forge/actions/runs/38723943582)
confirmed the original Forge `8975f234` baseline: all 140 cases passed
(liboauth2 18, token verifier 8, trusted subject 21, HTTP auth 7, patched-library
regressions 86), with the installed package consumer and dependency linkage
also passing. That run's candidate was the superseded Forge `7f1043eb` and
stopped during configure on the Jansson release/ABI-version bug described
below. It is not a regression-test result for the current candidate.

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
