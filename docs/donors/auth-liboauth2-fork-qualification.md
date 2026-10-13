# Patched liboauth2 F0 Qualification

Date: 2026-10-13. **PASS for the strict-profile public-API F0 matrix on macOS
and Linux. Overall backend/delivery qualification remains PENDING.** This report
does not approve adapter TLS/lifecycle or full package/export acceptance.
The [original upstream report](auth-liboauth2-qualification.md) remains unchanged
historical evidence; its findings are not reclassified by this fork run.

The [upstream-ready defect report](liboauth2-upstream-report.md) separates the
reproduced bugs from optional controls and remains **NOT_SUBMITTED**.

## Source And Dependencies

The fork is `vendor/liboauth2`, upstream version 2.4.0, base commit
`85da4346435aec77f2ff11b529f2c1e8e5033c44`, with the resource-controls patch committed
as **`ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5`** on `resource-verification-controls`.
Local HEAD and its upstream parent were verified; the fork worktree is clean.
The coordinator pushed this exact commit to the fork's
`resource-verification-controls` branch; no upstream branch was rewritten.

The F0 evidence below predates the commit and exercised the frozen working patch.
The Linux test-source copy's `src`, `include` and `test` trees now match the
committed fork byte-for-byte. Recording the immutable pin is not a new runtime run.

Recorded source pins follow [PROVENANCE](../../PROVENANCE.md); dependency versions
below are confirmed by the two F0 CMake caches and private-prefix package metadata.
The probe additionally checks the CJOSE header/runtime version on every case.

| Dependency | macOS | Linux | Recorded Source |
|---|---|---|---|
| liboauth2 | 2.4.0 + resource controls | 2.4.0 + resource controls | Fork `ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5`, parent `85da4346435aec77f2ff11b529f2c1e8e5033c44` |
| CJOSE | 0.6.2.8 | 0.6.2.8 | Unforked upstream `e787ace6a203d483038a793fd58f958a2f9b48c1` |
| Jansson | 2.14.1 | 2.14.1 | `ed5cae4ed0621ef409510f94270c9f8f263736d0` |
| OpenSSL | 3.6.4 | 3.5.5 | Platform-selected dependency |
| libcurl | 8.7.1 | 8.18.0 | Platform-selected dependency |
| PCRE2 | 10.47 | 10.46 | Platform-selected dependency |

Both consumers use Clang 22.1.8. This is a build/runtime evidence record, not
whole-dependency license/advisory clearance or a reproducible release lock.

## Qualified Profile And Matrix

**`verify.cache=none` is mandatory in every qualified configuration and clone.**
Legacy/default decision-cache behavior is not qualified. JWKS caching is distinct
from caching an authentication decision; the fixture separately tests its disable,
expiry, isolation and bounded refresh behavior. Redirects are explicitly disabled;
timeouts, retry counts, response sizes and the exercised key/refresh controls are
set through public vendor options, without private-header or replacement-engine use.

| Public-API Gate | Exercised Evidence | macOS | Linux |
|---|---|---|---|
| JWT claims / algorithm / key policy | Genuine RSA signatures; issuer/audience/exp/nbf; missing, duplicate and wrong-type claims; audience arrays; HS/RSA/EC confusion; `alg=none` | PASS | PASS |
| Protected access-token purpose | Same issuer/key/audience; ID-token and missing-typ denial; strict `at+jwt` through public CJOSE over the same verified bytes | PASS | PASS |
| Issuer/policy cache isolation | Two issuers with the same kid and different keys; cross-issuer/forged-key denial; shared-URI distinct issuer policies | PASS | PASS |
| Cache disable / expiry | Identical opaque token revalidation; inactive/expired denial; JWT expiry with warmed JWKS; non-token input denial after warm-up | PASS | PASS |
| Real opaque introspection | Actual HTTP POST; active/inactive; issuer/audience/subject/expiry; duplicate/wrong-type response claims | PASS | PASS |
| JWKS parsing / bounds | Malformed/oversized bodies; max and max+1 key entries, including unusable entries; first-fetch and refresh bounds | PASS | PASS |
| Rotation / bounded refresh / clones | Rotation, unknown-kid storm, cooldown expiry, one-flight coalescing, typed lock-wait timeout, long successful fetch and later clone usability | PASS | PASS |
| Dependency failure classification / containment | Transport reset versus HTTP/timeout/invalid; valid-body JWKS 503 at first fetch and refresh; 307/308 no credential forwarding; NULL-status cannot bypass non-2xx | PASS | PASS |

Python supplies bounded loopback servers, counters and boolean observations only;
C++ uses disposable keys and public CJOSE/liboauth2 APIs. No real credentials or
raw token/assertion bodies are emitted. Harness failure suppresses all PASS flags.

## Executed Evidence

Target: `forge_auth_liboauth2_patched_qualification`; CTest:
`test_forge_auth_liboauth2_patched_regression`; standalone configuration:
`FORGE_QUALIFY_PATCHED_LIBOAUTH2=ON`. One CTest contains the 86 cases, not 86 CTests.

| Run | Result | Test Duration | Summary Artifact |
|---|---|---|---|
| macOS strict-profile F0 | 86/86 cases, 1/1 CTest | 23.57 s | [F0 log](../../build-auth-resource/f0-expanded-test.log) |
| Linux strict-profile F0 | 86/86 cases, 1/1 CTest | 22.53 s | [F0 log](../../build-auth-resource/linux/f0-expanded-test.log) |
| Linux upstream-derived Check suite on patched fork | 135 checks; 0 failures/errors | Not recorded here | [Check log](../../build-auth-resource/linux/liboauth2-check/check_liboauth2.log) |
| macOS adapter and `test_forge_package_install` target | Build/link and install completed; runtime acceptance PENDING | Not recorded here | [Build/install log](../../build-auth-resource/adapter-package-build.log) |

The Check run includes the CJOSE-8-compatible 32-byte HMAC test-fixture updates;
it is not a verdict for pristine upstream or evidence of Forge adapter lifecycle.

Per-case JSON and all eight feature gates were read from
[macOS LastTest](../../build-auth-resource/f0/Testing/Temporary/LastTest.log) and
[Linux LastTest](../../build-auth-resource/linux/f0/Testing/Temporary/LastTest.log).
Both have 86 distinct PASS records, `fixture_valid=true`, `regression_passed=true`,
`f0_public_api_ready=true`, and `result_cache_disabled=true` for every case.
Artifacts are generated/disposable; retain these results and their fingerprints
before cleaning the build directories.

| Evidence | SHA-256 |
|---|---|
| macOS F0 summary log | `1dfea4a9c4b94e56524559dec1eb5ddd186bedeeb2189e978804e985fc32960b` |
| Linux F0 summary log | `141701b90aaf07b0a6cb2319fa1b34842312751be79b36bd292c46dbcab37747` |
| macOS per-case LastTest | `f839a5dd8b9ec21f3048607138a701b31cdadd0aeb0324476c8b6c3800d21d8d` |
| Linux per-case LastTest | `e37df0e0ccfd5cbc7d70f444b2a2354f33470ebc7c40f39c0417f6ca3c40b937` |
| Linux Check log | `750d267ddcf5d9aa1036066515a6e3deb0e3cbc61f94d040410c806c5e2a20e2` |
| macOS adapter/package build log | `30f4bb476435052da2bbaa1f3acb5cb7599280eb1376439873144e7ff023b1e0` |

## Remaining Acceptance

The macOS adapter and package-install target have completed their build; the log
confirms adapter/test linking and installation. This is **build/install evidence,
not an executed adapter runtime or installed-consumer test verdict**.

Adapter TLS/trust and lifecycle integration acceptance, package consumer/export
acceptance, and dependency license/advisory closure are **PENDING in this report**.
They require separate coordinator evidence; this loopback F0 matrix does not prove
them. The successful macOS/Linux runs do provide the cross-platform F0 evidence
even though each raw runner summary still lists coordinator-owned aggregation gates.

Before Forge delivery: retain the source/test manifest and attach the separate
adapter runtime and package-consumer results. The fork pin is already available
from the remote; that publication does not approve the Forge adapter.
No production-readiness, release, consumer-pin or overall qualification approval
is implied by this report.
