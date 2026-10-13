# liboauth2 F0 Qualification

Date: 2026-10-13. Candidate: **NOT QUALIFIED**.
Evidence: source inspection and eight executed macOS characterization cases.
Two independent Sol 6.1 xHigh reviewers confirmed the source-level blockers
and issued **CLEAN for the fixture/evidence only** after one diagnostic fix.
No backend or Auth/API feature approval is claimed.

## Provenance And Dependencies

- Upstream: OpenIDC/liboauth2, version 2.4.0, candidate [`85da4346435aec77f2ff11b529f2c1e8e5033c44`][candidate]. The inspected
  checkout HEAD matches this SHA and was clean. This is **not an approved vendor pin**.
- [configure.ac:16-34][dependencies] requires OpenSSL, libcurl, Jansson, cjose
  and PCRE2 (`libpcre2-8`). [Package metadata:14][package] is not an exact lock. Dependency versions/pins and platform closure
  must be recorded with probe/build evidence; they are not qualified here.
- Candidate [LICENSE][license] is Apache-2.0. Dependency licenses, advisories, transitive closure and update policy remain pending; not a whole-graph
  license or advisory clearance. Disable optional cache/Apache/NGINX bindings
  for the minimal consumer, then verify actual exported dependencies.

## Definitive Source Gaps

### F0 Error Classification: FAIL

[JWKS URI resolver:2324-2375][jwks] stores HTTP status only in a local variable
and returns response bytes, not transport/status evidence. The
[JWT callback:1556-1577][jwt-callback] accepts a status output but does not set
it or propagate key-resolution failures. [token_verify:963-965][normalization]
normalizes failure with status outside 400-599 to 401. Thus JWKS cache-miss /
failed key resolution cannot reliably distinguish issuer unavailability from
invalid credentials through this resource-verifier path without log parsing.

Do not generalize this to every outage: [introspection:353-360][introspection]
passes the HTTP status through; a real HTTP 503 survives normalization.
For a timeout **before response headers**, [http.c:1165-1199][timeout] assigns
504, then overwrites it with the response code 0; token_verify turns 0 into 401.
Low-level HTTP exposes **bool plus status**, not a generic bool-only API.
A valid cached key successfully verifying a token during outage is not a
failed key-resolution case. The fresh-process failure probes below exercise
these paths; cached-success outage behavior has not been tested.

### F0 Redirect And Credential Boundary: FAIL

[http.c:1048-1049][redirect] unconditionally enables FOLLOWLOCATION and five
redirects. Protocol restrictions do not enforce same-origin destinations.
[POSTFIELDS:1118-1120][post] and the [introspection POST][introspection] place
the presented token in the request body. The executed cross-origin 307/308
cases forwarded both the fabricated token and the `client_secret_post` secret
to a second loopback server. The source-confirmed gap is absence of
redirect/origin policy. This is not a claim that every Authorization header is
forwarded, or that an unauthenticated caller controls the configured endpoint.

[Endpoint options:360-389][endpoint-options], their [implementation][endpoint]
and [public HTTP context:530-589][http-context] expose no resource-verifier
option to disable following or restrict redirect origin. A trusted initial URL
alone does not close this boundary. No replacement HTTP/introspection engine
or private-internals workaround is approved.

## Scope Nuances And Mandatory Matrix

Public local-JWK configuration can support JWT composition with host-controlled
fetching, but a host JWKS engine/introspection replacement is not approved and
does not fix the existing redirect path. Protected `typ` is **not** a definitive
blocker: [CJOSE protected-header access][protected-header] over the same immutable
verified bytes is a supported composition route; purpose tests remain NOT_RUN.

| Mandatory F0 requirement | Source result | Runtime evidence |
|---|---|---|
| Linux/macOS minimal consumer, toolchains, package/export | Pending | macOS standalone consumer built; Linux/package NOT_RUN |
| Exact dependencies, licenses, advisories, update policy | Pending closure | NOT_RUN |
| JWT issuer/audience, algorithm/key, exp/nbf, rotation | Pending qualification | NOT_RUN |
| Purpose/typ; same issuer/key/audience, ID-token rejection | Supported route; pending policy tests | NOT_RUN |
| Opaque introspection active/resource/subject/expiry, no JWT fallback | Pending qualification | NOT_RUN |
| Rejection versus outage/timeout/key-resolution failure | FAIL: status evidence lost on paths above | Reproduced; actual introspection 503 survives |
| Configured destinations, redirect/origin credential containment | FAIL: unconditional following | 307/308 body forwarding reproduced |
| Token/response/claims/JWKS size and key-count limits | Pending qualification | NOT_RUN |
| Finite timeout/retry bounds, queue, cancellation/drain | Pending; status-loss gap above | NOT_RUN |
| Memory ownership, thread safety, shared config | Pending qualification | NOT_RUN |
| Cache isolation/freshness, expiry, unknown-key coalescing | Pending qualification | NOT_RUN |
| Secret-safe logs/errors/cache keys | Pending qualification | NOT_RUN |
| Same kid across issuers, wrong aud, inactive/unknown key, oversize/outage | Pending negative fixtures | NOT_RUN |

## Executed Characterization

Fixture: [standalone probe and runner](../../tests/auth/liboauth2_qualification/README.md).
Preserved [JSON observations](auth-liboauth2-qualification-results.jsonl) were
extracted verbatim from the successful CTest output; generated binaries/logs
are disposable after review.
Platform: macOS 26.1 (25B78), arm64, Homebrew Clang 22.1.8, CMake/Ninja,
Python 3.14.6. Source-built dependencies were installed in one private generated
`build-auth-f0` directory; existing system dependencies were only read. No
system packages were installed or modified.

| Dependency | Tested source/version |
|---|---|
| liboauth2 | 2.4.0, `85da4346435aec77f2ff11b529f2c1e8e5033c44` |
| CJOSE | 0.6.2.5, `df4a5de3a344d202c218c7e4e02bfa43efc785be` |
| Jansson | 2.14.1, `ed5cae4ed0621ef409510f94270c9f8f263736d0` |
| OpenSSL | Existing Homebrew 3.6.4, explicitly selected instead of system pkg-config's OpenSSL 4 |
| libcurl | System package metadata 8.7.1, linked `/usr/lib/libcurl.4.dylib` |
| PCRE2 | Existing Homebrew 10.47 |

liboauth2/CJOSE used their upstream Autotools builds, without tracked source
changes; Jansson used its upstream CMake build. Optional Apache, memcache,
Redis and jq bindings were disabled. Upstream dependency unit suites were not
run; this does not establish complete dependency/platform qualification.

`cmake --build build-auth-f0/probe -j4` succeeded.
`ctest --test-dir build-auth-f0/probe --output-on-failure -V` reproduced the
expected gaps: one characterization test containing eight cases, 23.12 seconds
initially and 23.10 seconds after the review fix; JSON observations were identical.
Its successful exit means **bugs reproduced**, not **backend accepted**.

| Case | Accepted | Public status | Additional observation |
|---|---|---|---|
| Invalid local JWT | false | 401 | Deliberately invalid signature, baseline rejection |
| Fresh JWKS endpoint returns 503 | false | 401 | Request reached endpoint |
| Fresh JWKS endpoint unreachable | false | 401 | Reserved non-listening local port |
| Introspection `active=false` | false | 401 | Request reached endpoint |
| Introspection endpoint returns 503 | false | 503 | Negative control: HTTP status retained |
| Introspection timeout before headers | false | 401 | Request reached endpoint |
| Introspection redirect 307 | false | 401 | Second origin received POST, token and client secret |
| Introspection redirect 308 | false | 401 | Second origin received POST, token and client secret |

The runner summary reported `fixture_valid=true`, `gap_reproduced=true`,
`http_503_control_preserved=true` and `backend_approved=false`. Only fabricated
credentials were used; neither their values nor vendor diagnostics are emitted.
Before the successful run, two attempts failed in the harness because Python's
HTTPServer performed reverse DNS during construction. A process sample
confirmed `socket_gethostbyaddr`; the isolated fixture now binds through
TCPServer without resolving a hostname. Those attempts are harness failures,
not backend results. HTTPS, valid-token outage behavior, complete claims/purpose
policy, concurrency, revocation and Linux remain **NOT_RUN**.

Review found that individual reproduction flags could be true even when the
overall measurement was invalid. All such flags now also require fixture
validity. A separate negative harness run suppressed recording of the timeout
request while executing the same probes: the runner returned 2 and every
summary evidence flag was false. This checks diagnostic honesty, not backend
security. Both reviewers accepted the fix; the coordinator executed the tests.

Additional checks passed: `tests/structure/check_structure.py .` (2432 files),
`tests/structure/net_plugins_tests.py` (23 tests), and whitespace checks for
the pending files. The production feature/package suites were not run because
no production implementation or package dependency was added.

## Reuse Handoff: Pending Implementation

- Move common credential ID/binding/state/record/scopes and validation from [pairing types:55](../../libraries/auth/pairing/include/forge/auth/pairing/types.cppm)
  and [pairing.cpp:171](../../libraries/auth/pairing/pairing.cpp); bootstrap,
  pending approval and token issuance stay with pairing. No aliases/mirrors.
- Consolidate [session.cpp:57](../../libraries/auth/session/session.cpp) common
  validation; retain session matching at 192 and the existing session engine.
  Preserve credential/session/device/pending Raw bytes with legacy-writer fixtures; consumers separately prove old DB reopen. Update package closure and structure
  checks that currently require session -> pairing, not just the leaf dependency.
- Reuse [trusted invocation](../../libraries/api/core/include/forge/api/core/trusted_invocation.cppm)
  and [server_supplied](../../libraries/api/core/include/forge/api/core/server_supplied.cppm).
  [HTTP provider sites:1932/2046/2130](../../libraries/api/http/include/forge/api/http/binding.cppm): keep caller/subject separate, reject conflicting trusted values, bound stream freshness/revoke.
- Reconcile exact existing [c971faaf2b3ce73e18f901a9ba6169d1315efc8c][guard]
  (18 files, reachable via 1b98c5e2, absent from current local branches).
  Do not reimplement websocket_guarded. Extend [WS accept:99](../../libraries/api/websocket/binding.cpp) to pass captured context into
  [serve_stream](../../libraries/api/stream/include/forge/api/stream/server.cppm).
  Authentication is not a business principal; no dummy modules/parallel engines.

## Stop Decision

Mandatory source FAIL already stops production backend integration and the Gate
pin update under the [iteration plan](../iterations/auth-api-resource-verification-v1.md).
Implementation stays pending behind F0. Probes refine evidence, not waive missing supported policy/error APIs.
Require an upstream fix or separately approved backend selection, then repeat qualification and review.

[candidate]: https://github.com/OpenIDC/liboauth2/tree/85da4346435aec77f2ff11b529f2c1e8e5033c44
[dependencies]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/configure.ac#L16-L34
[package]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/liboauth2.pc.in#L14
[license]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/LICENSE
[jwks]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/jose.c#L2324-L2375
[jwt-callback]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/jose.c#L1556-L1577
[normalization]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/oauth2.c#L963-L965
[introspection]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/oauth2.c#L335-L360
[timeout]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1155-L1199
[redirect]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1048-L1067
[post]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1118-L1120
[endpoint-options]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/include/oauth2/cfg.h#L360-L389
[endpoint]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/cfg/proto_cfg.c#L98-L193
[http-context]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/include/oauth2/http.h#L530-L589
[protected-header]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/jose.c#L1470-L1476
[guard]: https://github.com/SystasisIO/forge/commit/c971faaf2b3ce73e18f901a9ba6169d1315efc8c
