# liboauth2: Introspection Redirect Exposure And Lost Verification Failure Evidence

Submission status: **NOT_SUBMITTED**. Prepared: 2026-10-14.
This is a draft for later maintainer review, not an issue, pull request, advisory,
severity assignment, or request for a vulnerability identifier. Nothing was
submitted upstream as part of preparing this document.

## Summary And Evidence Boundary

Two defects were reproduced against the exact upstream revision below:

1. Introspection follows redirects unconditionally. A cross-origin HTTP 307 or
   308 forwards the POST body, including the presented token and a
   `client_secret_post` client secret, with no public resource-verifier option
   to disable following or require the same origin.
2. Verification loses dependency failure evidence: a pre-header introspection
   timeout and a failed fresh JSON Web Key Set (JWKS) fetch become `false/401`,
   indistinguishable from credential rejection through the public verifier.
   The existing resource endpoint timeout/retry options are also not applied
   by these paths.

These are historical findings for **liboauth2 2.4.0 at
`85da4346435aec77f2ff11b529f2c1e8e5033c44`**, not a claim about current upstream
HEAD or every release. Current upstream branches, releases, issues, and fix
status were **not checked** while preparing this draft. Recheck before submission.

The downstream patch has already been committed in a maintained fork. The
2026-10-13 qualification records supply the runtime observations below; this
documentation pass inspected the immutable patch, fixture sources, and reports
but **did not build or execute tests**. Historical results are not new runs.
Passing the loopback regression matrix does not establish production, TLS,
adapter lifecycle, package/export, or full dependency qualification.

## Exact Revisions And Recorded Environment

| Item | Revision / Version |
|---|---|
| Affected upstream source | [OpenIDC/liboauth2 v2.4.0][upstream], `85da4346435aec77f2ff11b529f2c1e8e5033c44` |
| Maintained fork | [SystasisIO/liboauth2, resource-verification-controls][fork] |
| Inspected patch | [ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5][patch], immediate parent is the affected upstream SHA |
| Standalone fixture reference | [Forge fixture at 19cb910e3200944ca4dcc321b80bb8986ada63b2][fixture-pin] |
| Original characterization | macOS 26.1 (25B78), arm64; Clang 22.1.8; Python 3.14.6; CMake/Ninja |
| Original dependencies | CJOSE 0.6.2.5 (`df4a5de3a344d202c218c7e4e02bfa43efc785be`), Jansson 2.14.1 (`ed5cae4ed0621ef409510f94270c9f8f263736d0`), OpenSSL 3.6.4, libcurl 8.7.1, PCRE2 10.47 |
| Patched qualification dependencies | CJOSE 0.6.2.8 ([e787ace6a203d483038a793fd58f958a2f9b48c1][cjose-pin]), Jansson 2.14.1 ([ed5cae4ed0621ef409510f94270c9f8f263736d0][jansson-pin]); OpenSSL 3.6.4/macOS, 3.5.5/Linux; libcurl 8.7.1/macOS, 8.18.0/Linux; PCRE2 10.47/macOS, 10.46/Linux |

The inspected Forge gitlink and the clean vendor checkout both point to the
patch SHA. Both base and fork still report version `2.4.0`; the version string
alone cannot distinguish them. Preserve source SHA and dependency provenance.
The patch spans 25 files and combines defect fixes with additional controls;
it is not a minimal two-bug patch. Upstream may prefer separately reviewed
changes. The upstream Apache-2.0 license and attribution are retained.

## Defect 1: Cross-Origin Introspection POST Forwarding

### Prerequisites And Configuration

A resource server uses `oauth2_cfg_token_verify_add_options()` with method
`introspect`, an operator-configured endpoint, and endpoint authentication
`introspect.auth=client_secret_post`. The library places both the presented token
and the configured client secret in its form-encoded POST body. The configured
endpoint returns a 307 or 308 with a `Location` at another origin.

The fixture uses two servers bound to `127.0.0.1` on different ephemeral ports;
the port difference makes them different origins. It uses only the synthetic
token `forge-f0-fabricated-token`, client ID `forge-f0-client`, and secret
`forge-f0-fabricated-client-secret`. Do not replace them with real credentials.

### Cause, Expected Behavior, And Impact

[Upstream http.c:1048-1067][upstream-redirect] enables `CURLOPT_FOLLOWLOCATION`
and five redirects unconditionally. Restricting redirect protocols to HTTP/HTTPS
does not restrict origins. [POST handling][upstream-post] and
[introspection construction][upstream-introspect] put credentials in the body.
The public [endpoint options][upstream-endpoint] and
[HTTP call context][upstream-http-api] expose no redirect/origin control for this
path. Disabling following is therefore unavailable through this public API.

Expected: a credential-bearing verifier call can refuse redirects before any
second-origin request. Observed: the second server receives the POST, token,
and client secret for both 307 and 308. Its `active=false` reply still produces
`false/401`; credential exposure occurs even though authentication is denied.

In deployment, exposure requires a redirect from the configured endpoint and
a reachable destination. Potential causes include endpoint compromise or
misconfiguration; the fixture does not establish an unauthenticated attacker's
ability to control that endpoint. There is no claim that every Authorization
header, authentication method, redirect status, or production deployment leaks.
The demonstrated property is POST-body forwarding with `client_secret_post`.
HTTPS forwarding, TLS downgrade behavior, arbitrary-network access, token
validity, and subsequent credential abuse were not demonstrated by this HTTP
loopback fixture. A trusted initial URL does not itself constrain a redirect.

## Defect 2: Timeout And JWKS Outage Become Credential Rejection

### Prerequisites And Cause

The characterization invokes public `oauth2_token_verify()` in a fresh process
per case. `jwks_uri` therefore starts without a warmed key cache. The JWKS
token is a compact JSON Web Token (JWT) with a deliberately invalid signature;
these probes measure failed key-fetch reporting, not admission of a valid token.
For introspection, the server reads the request and deliberately sends no headers.

Two source paths lose evidence:

- [http.c:1155-1199][upstream-timeout] sets status 504 on a curl timeout, then
  overwrites it with `CURLINFO_RESPONSE_CODE`, which is 0 before headers.
- [The JWKS URI resolver][upstream-jwks] keeps HTTP status local and returns
  response bytes rather than failure evidence. [The JWT callback][upstream-jwt]
  does not populate its status output. [token_verify normalization][upstream-normalize]
  turns failure with status outside 400-599 into 401.

The JWKS resolver also returns allocated response bytes on its non-2xx failure
branch instead of discarding them. The fork adds valid-key-body/503 regression
cases to ensure such a body is never used to authenticate. The eight original
probes did **not** measure valid-token admission against upstream on that path;
do not present the fork's negative regression as an upstream exploit result.

The original public configurations request `introspect.http_timeout=1` or
`jwks_uri.http_timeout=1`, and zero retries. Source inspection shows these
resource-verifier paths apply TLS/proxy settings but not those timeout/retry
settings. The historical timeout probe consequently uses the HTTP context's
15-second default, not the requested one second. Timeout is not retried by this
candidate. Honoring already exposed options is a correctness fix, distinct from
introducing new response-size or refresh controls.

### Impact And Limits

Callers cannot reliably distinguish an invalid credential from dependency
unavailability using the public verifier's boolean/status pair on these paths.
This can produce inappropriate authentication challenges, misleading metrics,
or incorrect application recovery decisions. It does not by itself demonstrate
authentication bypass or establish a universal retry policy.

A real introspection HTTP 503 **does survive** as `false/503` and is a negative
control. Do not claim all service failures collapse to 401, or describe the
low-level HTTP API as boolean-only. Successful validation with an already cached
key during an outage is not failed key resolution and was outside the original
characterization. The unreachable case reserves a non-listening loopback port;
it is not a DNS, HTTPS, or Internet outage test.

## Historical Expected / Observed Results

These are the eight preserved macOS observations from 2026-10-13. Expected
behavior describes the boundary requirement, not an API that upstream then had.

| Fixture Case | Expected | Observed At Affected SHA |
|---|---|---|
| `invalid-jwt` | Ordinary credential rejection | `false/401` (control) |
| `jwks-503` | Preserve endpoint 503 separately from credential rejection | `false/401`; endpoint reached |
| `jwks-unreachable` | Preserve transport failure separately from credential rejection | `false/401` |
| `inactive` | Ordinary credential rejection for `active=false` | `false/401` (control) |
| `introspection-503` | Preserve endpoint 503 | `false/503` (control) |
| `introspection-timeout` | Preserve timeout; honor explicit timeout | `false/401`; request reached; 15-second default used |
| `redirect-307` | Ability to refuse redirect; no second-origin credentials | `false/401`; second origin received POST, token, and secret |
| `redirect-308` | Ability to refuse redirect; no second-origin credentials | `false/401`; second origin received POST, token, and secret |

The original CTest contained eight cases, completed in 23.12 seconds, then
23.10 seconds after a diagnostic fix. Summary: `fixture_valid=true`,
`gap_reproduced=true`, `http_503_control_preserved=true`, `backend_approved=false`.
The enduring observations are [JSONL](auth-liboauth2-qualification-results.jsonl);
the [historical qualification](auth-liboauth2-qualification.md) records scope and
harness fixes. Its successful test exit means **bugs reproduced**, not approval.

## Standalone Reproduction Commands

### Prerequisites

Use a POSIX Linux/macOS host with Git, a C compiler, a C++23 compiler (recorded
qualification used Clang 22.1.8), Autoconf/Automake/Libtool, Make, pkg-config,
CMake >= 3.31, Ninja, and Python 3. Development packages must expose OpenSSL 3,
libcurl, Jansson, CJOSE, and PCRE2 to pkg-config. The patched standalone CMake
lane requires **Jansson 2.14.1, CJOSE 0.6.2.8, and PCRE2 >= 10.42**. Obtain them
from ordinary packages or independently built public sources at the pins above;
set `PKG_CONFIG_PATH` and `CMAKE_PREFIX_PATH` for any user-selected prefix.
Select one consistent OpenSSL for CJOSE, liboauth2, and CMake.

No historical `build-auth-*` directory, private prefix, prebuilt probe, Forge
module build, or production server is required. The existing
[standalone CMake project](../../tests/auth/liboauth2_qualification/CMakeLists.txt)
and runners are used directly. The patched lane also uses checked-in
`cmake/ForgeOAuth2.cmake`, `FindOAuth2Dependencies.cmake`, and
`ForgeVendorCompilePolicy.cmake`; no vendor private headers are included by probes.

Run from the Forge repository containing the fixture at the reference above.
The following commands are provided for later execution; they were **not run**
in this documentation pass. They intentionally use CJOSE 0.6.2.8 rather than
reinstalling the historical 0.6.2.5 dependency. Thus they are a fresh reproduction
recipe, not a rerun of the exact original dependency manifest. Record actual
dependency versions, source SHAs, link paths, and results with any new run.

```sh
set -eu
FORGE_ROOT="$PWD"
WORK="$(mktemp -d)"
if [ "$(uname -s)" = Darwin ]; then
  export CC="$(brew --prefix llvm)/bin/clang"
  export CXX="$(brew --prefix llvm)/bin/clang++"
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
else
  export CC="${CC:-clang}"
  export CXX="${CXX:-clang++}"
fi
pkg-config --exists 'openssl >= 3' 'openssl < 4' libcurl 'jansson = 2.14.1' \
  'cjose = 0.6.2.8' 'libpcre2-8 >= 10.42'
pkg-config --modversion openssl libcurl jansson cjose libpcre2-8
git rev-parse HEAD
```

On macOS, select the dependency pkg-config directories explicitly if they are
not already discoverable; selecting the compiler alone does not select OpenSSL.
Keep both reproduction lanes in this shell so `WORK` and dependency selection
remain consistent. Neither lane requires modifying tracked source.

### A. Reproduce The Historical Upstream Behavior

Build the exact upstream source in a new temporary directory and install only
there. Do not switch or reset the existing vendor checkout.

```sh
git clone https://github.com/OpenIDC/liboauth2.git "$WORK/upstream"
git -C "$WORK/upstream" checkout --detach 85da4346435aec77f2ff11b529f2c1e8e5033c44
test "$(git -C "$WORK/upstream" rev-parse HEAD)" = 85da4346435aec77f2ff11b529f2c1e8e5033c44
(
  cd "$WORK/upstream"
  ./autogen.sh
  ./configure --prefix="$WORK/upstream-prefix" --libdir="$WORK/upstream-prefix/lib" \
    --without-apache --without-memcache --without-redis --without-jq
  make -j4
  make install
)
export PKG_CONFIG_PATH="$WORK/upstream-prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$WORK/upstream-prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test "$(pkg-config --variable=prefix liboauth2)" = "$WORK/upstream-prefix"
pkg-config --modversion liboauth2
cmake -S "$FORGE_ROOT/tests/auth/liboauth2_qualification" -B "$WORK/characterization" -G Ninja \
  -DFORGE_QUALIFY_PATCHED_LIBOAUTH2=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_OSX_SYSROOT="${SDKROOT:-}" \
  -DCMAKE_BUILD_RPATH="$WORK/upstream-prefix/lib"
cmake --build "$WORK/characterization" -j4
ctest --test-dir "$WORK/characterization" \
  -R '^test_forge_auth_liboauth2_characterization$' --output-on-failure -V
```

The runner starts its own bounded loopback origin/receiver and the reserved
non-listening socket; no manual server setup is needed. It launches a fresh
probe process per case. Inspect CMake/pkg-config link selection to ensure no
system or patched liboauth2 was substituted.

For direct sanitized JSON output instead of CTest, run:

```sh
python3 "$FORGE_ROOT/tests/auth/liboauth2_qualification/run.py" \
  --probe "$WORK/characterization/forge_auth_liboauth2_qualification"
```

Runner exit 0 means the expected historical gaps reproduced; exit 1 means a
valid measurement differs; exit 2 means a harness/configuration/probe failure.
CTest reports a failed test for either nonzero runner exit, so inspect the JSON
to distinguish them. Never reinterpret exit 1 as approval or exit 2 as a product
defect. The probe's 20-second alarm, runner's 35-second work deadline, and
bounded teardown are harness limits, not successful cancellation guarantees.

### B. Regress The Exact Fork Through Public APIs

This lane builds the vendored C library and existing patched probe directly,
without relying on the liboauth2 installed in lane A. First require the exact
clean vendor pin; stop rather than altering it if these checks fail.

```sh
test "$(git -C "$FORGE_ROOT/vendor/liboauth2" rev-parse HEAD)" = ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5
test -z "$(git -C "$FORGE_ROOT/vendor/liboauth2" status --porcelain)"
cmake -S "$FORGE_ROOT/tests/auth/liboauth2_qualification" -B "$WORK/patched" -G Ninja \
  -DFORGE_QUALIFY_PATCHED_LIBOAUTH2=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_OSX_SYSROOT="${SDKROOT:-}"
cmake --build "$WORK/patched" -j4
ctest --test-dir "$WORK/patched" \
  -R '^test_forge_auth_liboauth2_patched_regression$' --output-on-failure -V
```

Alternatively run `patched_run.py --probe` with
`$WORK/patched/forge_auth_liboauth2_patched_qualification` for sanitized JSON.
Here exit 0 means the regression matrix passed, 1 a regression failure, and 2
a harness error. Expect 86 case records, all PASS, `fixture_valid=true`,
`regression_passed=true`, and `f0_public_api_ready=true`. These describe only the
standalone public-API matrix; inspect its separate `coordinator_not_run` gates.

## Patched Public API Semantics And Regressions

The fork adds `oauth2_token_verify_ex()` and HTTP `_ex` calls with optional
caller-owned result structs. Evidence is per call, without borrowed strings or
global/thread-local error state. `result.status_code` retains the received
endpoint status; 0 can mean no headers, local verification, or a cache hit.
It is separate from the legacy normalized status output.

A completed HTTP exchange with status 503 still returns true from the low-level
HTTP call with `error=OAUTH2_HTTP_CALL_OK`; it must return false from token
verification. Non-2xx responses are rejected even with `status_code=NULL`, and
failed responses are not passed on as authenticated payloads. Endpoint 401 is
`OAUTH2_TOKEN_VERIFY_HTTP_ERROR`, not `CREDENTIAL_REJECTED`. If several methods
are configured, a later success wins; if all fail, endpoint failure evidence
survives a later ordinary rejection. Legacy signatures remain wrappers.

The qualified profile explicitly sets `verify.cache=none`, endpoint-prefixed
`http_follow_redirects=false`, positive `http_timeout`, `http_retries=0`, and
`http_max_response_bytes=4096` in these tests. For example, introspection uses
`introspect.http_timeout=1&introspect.http_retries=0&introspect.http_follow_redirects=false`;
JWKS uses the corresponding `jwks_uri.` prefix. Fixture configurations are
cloned before use. Endpoint URLs and CA configuration remain operator-owned;
production must keep TLS peer/hostname verification enabled.

**The fork does not add a same-origin redirect allowlist and does not disable
following by default.** Defaults remain following=true, maximum five redirects,
and a 1 MiB body cap. Credential containment depends on explicitly disabling
redirects. A positive redirect count is not an origin restriction. Zero HTTP
timeout is rejected; legacy source signatures do not imply unrestricted
behavioral or binary compatibility.

| Regression Case | Required Fork Result |
|---|---|
| `redirect-307`, `redirect-308` | false; `HTTP_ERROR`, endpoint status 307/308; normalized 401; zero forwarded requests/token/secret |
| `jwks-503-first`, `jwks-503-refresh` | false; `HTTP_ERROR`, endpoint 503, normalized 503; valid key body must not bypass status |
| `jwks-timeout`, `introspection-timeout` | false; `TIMEOUT`, HTTP error TIMEOUT, endpoint 0, normalized 504; one request, explicit one-second timeout |
| `jwks-transport-reset`, `introspection-transport-reset` | false; `TRANSPORT_ERROR`, endpoint 0, normalized 502; zero retries |
| `introspection-401`, `introspection-503` | false; `HTTP_ERROR`, actual endpoint status retained; distinct from an inactive token |
| `introspection-inactive` | false; `CREDENTIAL_REJECTED`, endpoint 200, normalized 401 |
| `introspection-status-null` | Both extended and legacy verifier reject a 503 with an active-looking body and NULL status output |
| `http-503-status-null` | Low-level HTTP completion remains true/HTTP OK; endpoint status 503 retained in result |

Additional tests in the [patch's Check suite][patch] cover timeouts after
headers (raw endpoint 200, normalized 504), redirect-limit errors, exact and
over-limit response sizes, clone preservation of controls/CA settings, and
invalid numeric options. The multiple-method precedence rule above is documented
from the patched API/source, not claimed as a separately executed fixture case.
The [fork qualification](auth-liboauth2-fork-qualification.md) records 86/86 cases
in one CTest on each platform: macOS 23.57 seconds and Linux 22.53 seconds.
It also records 135 Linux Check checks with no failures/errors, including
CJOSE-8-compatible HMAC fixture updates. This is historical evidence, not a
new run or a pristine upstream Check-suite verdict.

## Additional Controls, Not The Two Defect Claims

The patch includes optional hardening and integration features that should be
reviewed separately rather than promoted into unmeasured upstream vulnerabilities:

| Feature | Purpose / Limits |
|---|---|
| Configurable HTTP response bound | The affected source already had a fixed 1 MiB cap; the fork makes the cap configurable and failure typed. This is not a claim of previously unlimited response buffering. |
| Explicit decision/URI cache disable | `verify.cache=none` disables cached verification decisions; `<type>.cache=none` disables URI caching. Decision and key caches are distinct. The tested strict profile requires decision-cache disable; legacy/default caching is not qualified. |
| JWKS key-count/refresh controls | `jwks_uri.max_keys` bounds the entire key array before import, including unusable entries. `refresh_interval` (seconds) throttles/coalesces attempts; `refresh_wait_timeout` (milliseconds) bounds clone lock waits. These are opt-in, reject explicit zero, and refresh controls are incompatible with `jwks_uri.cache=none`. |
| Clone-owned HTTP controls and CA bundle | Preserve operator configuration and limits across clones; not proof of full TLS integration acceptance. |
| Strict duplicate JSON rejection | Additional parser/response hardening with regression cases; no general upstream policy-bypass claim follows from its inclusion. |

With refresh controls, clones share provider-local refresh state, not a global
authentication-decision cache. An unexpired successful key snapshot can still
verify known keys after a failed refresh; unknown keys retain refresh failure
evidence during cooldown. Each token still needs signature and claims checks.
Rotation, unknown-key throttling, wait timeout, long fetches, and later clone
usability have separate fixture cases. Freeing an instance while it is in use
is unsupported.

Allowed algorithms, access-token purpose (`typ`), required claims, issuer,
audience, and freshness remain explicit consumer policy over verified bytes.
The fixture's protected-header checks use public CJOSE; these are not all newly
implemented vendor guarantees. Application queue bounds, overall deadlines,
cancellation, admission closure, and draining remain application responsibilities.

## Safe Evidence Handling And Submission Plan

The original runner uses fabricated constants; the patched runner uses synthetic
canaries and disposable signing keys. Both bind only to loopback, remove inherited
proxy routing for probes, suppress raw vendor/HTTP/child diagnostics, and emit
only validated statuses, counts, categories, and booleans. Neither prints tokens,
client secrets, assertions, private keys, or raw authenticated payloads.
Harness failure invalidates evidence flags instead of producing a security PASS.
Do not enable curl tracing, vendor debug logs, or real-provider traffic for this
reproduction. Sanitized evidence is sufficient; no production capture is needed.

Before any submission, separately authorize publication, inspect current upstream
fix/release/duplicate status, and choose the maintainers' appropriate reporting
channel for the credential-exposure finding. Refresh the exact-SHA reproduction
with safe dependencies and attach sanitized results. Keep defect fixes separate
from optional controls when proposing changes. This draft remains
**NOT_SUBMITTED** until an authorized submission and its reference are recorded.

## Criteria For Retiring The Fork

1. Identify an immutable official upstream revision/release with equivalent
   supported public redirect containment and failure-evidence behavior; verify
   actual source and APIs, not only a release note or version string.
2. Resolve every downstream-required extra control through upstream equivalents
   or an explicitly reviewed consumer-policy change. Do not silently drop limits,
   clone behavior, cache isolation, or the no-decision-cache requirement.
3. Repeat these regressions and the broader 86-case matrix on Linux and macOS,
   with recorded source/dependency manifests and relevant upstream Check tests.
   Preserve inactive/invalid controls, NULL-status behavior, raw versus normalized
   status, before/after-header timeouts, and zero redirect credential forwarding.
4. Complete separate dependency license/advisory, TLS/trust, adapter lifecycle,
   and installed package-consumer/export acceptance for the replacement pin.
5. Review and approve the vendor-pin/API migration and provenance update before
   removing the fork. No silent system-library substitution, permissive fallback,
   automatic pin update, or fork retirement is authorized by this report.

[upstream]: https://github.com/OpenIDC/liboauth2/tree/85da4346435aec77f2ff11b529f2c1e8e5033c44
[fork]: https://github.com/SystasisIO/liboauth2/tree/resource-verification-controls
[patch]: https://github.com/SystasisIO/liboauth2/commit/ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5
[fixture-pin]: https://github.com/SystasisIO/forge/tree/19cb910e3200944ca4dcc321b80bb8986ada63b2/tests/auth/liboauth2_qualification
[cjose-pin]: https://github.com/OpenIDC/cjose/tree/e787ace6a203d483038a793fd58f958a2f9b48c1
[jansson-pin]: https://github.com/akheron/jansson/tree/ed5cae4ed0621ef409510f94270c9f8f263736d0
[upstream-redirect]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1048-L1067
[upstream-post]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1118-L1120
[upstream-introspect]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/oauth2.c#L311-L360
[upstream-endpoint]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/include/oauth2/cfg.h#L360-L389
[upstream-http-api]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/include/oauth2/http.h#L530-L589
[upstream-timeout]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/http.c#L1155-L1199
[upstream-jwks]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/jose.c#L2318-L2375
[upstream-jwt]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/jose.c#L1556-L1577
[upstream-normalize]: https://github.com/OpenIDC/liboauth2/blob/85da4346435aec77f2ff11b529f2c1e8e5033c44/src/oauth2.c#L963-L965
