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
