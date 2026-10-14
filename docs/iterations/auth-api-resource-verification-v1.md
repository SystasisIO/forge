# Auth/API Resource Verification

Date: 2026-10-14.
Status: resource and final native gates PASS; current fixture cleanup complete; package verification and final delivery PENDING.
Hosted Linux snapshot: run 38483751720 FAILED; dependencies/catalog/regressions PASS; packages FAILED; native-negative SKIPPED. Package acceptance remains PENDING.
Current regressions: **Linux 11/11 PASS; macOS 14/14 PASS** after both repairs; twenty-repeat suites PASS on each platform.
Branch: `auth-api-resource-verification-v1`.
Published feature/workflow revision: `363423a83bb7f835e95baa0f40a03b3deb1b7d49`.
Current reviewed runtime revision: `b099d6aaca417c06bb4922abadf604cbbcfde638`, not `dev`.
Recorded native-cycle revision: `d5d29aac774eecd06a39371459cac5c5099c6c85`.
Unchanged `origin/dev`: `9eac8072575da0ccc1f1337aa456a1f0caecf25c`.
Starting development commit: `411644237490a63a52d1f810336673367c962651`.

## Current Outcome

Historical upstream qualification: the original liboauth2 candidate failed
mandatory error-classification and redirect containment requirements.
Source findings were independently confirmed by two
Sol reviewers and reproduced by eight standalone macOS cases. See the
[qualification report](../donors/auth-liboauth2-qualification.md) for exact
dependency provenance, observed results and remaining NOT_RUN coverage.

The user approved a maintained `SystasisIO/liboauth2` fork under `vendor/`.
Its `resource-verification-controls` branch starts at the exact qualified
upstream commit. The patch adds public transport controls and per-call failure
evidence, rather than replacing HTTP, JOSE or OAuth inside Forge. The published
fork commit `ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5` passed 86 focused
qualification cases on both macOS and Linux, plus 135 upstream-derived Check
tests on Linux. See the [fork report](../donors/auth-liboauth2-fork-qualification.md).
The original upstream failure report remains historical evidence, not a passing
test manifest for the fork.

The published `SystasisIO/oidc-agent` fork profile `5.3.8-systasis.1` is pinned at
`28d6f89701b64d5150b05d01f85b8a8eed54808e`. Its final 131-check fixture is RED
on pristine upstream (106 failures, 0 errors) and GREEN normally and under
ASan/UBSan (0 failures/errors). The signed-`time_t` P2 issue is CLOSED in source
and final tests; vendor/metadata and adapter review is scoped CLEAN. The actual
patched Linux daemon/library/Forge cycle and the final macOS AppAuth browser,
resume, post-expiry refresh and invalidate cycle PASS. See the
[oidc-agent report](../donors/oidc-agent-upstream-report.md), still **NOT_SUBMITTED**.

Native acceptance is closed, not an overall delivery verdict. The local
full-catalog Linux package build was **INTERRUPTED** by Docker Desktop shutdown
at 03:22 +05:00 on 2026-10-14; installed Forge verification remains
**PENDING**. After the WebSocket observation repair (`1f9fddc6`) and notification
race fix (`b099d6aa`), the latest regressions PASS: Linux 11/11, macOS 14/14,
plus twenty-repeat suites on each platform. The original failed runs remain
historical evidence. Current fixture cleanup is complete; generated build
files are intentionally retained until package evidence is complete. Docker is
stopped. On 2026-10-14 the user authorized GitHub Actions for the remaining
Linux verification instead of restarting the resource-heavy local container.
This authorization supersedes the earlier no-CI restriction for this feature;
it does not waive the package gate or authorize promotion before final review.
The feature branch has been pushed for hosted CI, not promoted to `dev`.
The latest reported run, `38483751720` at
`363423a83bb7f835e95baa0f40a03b3deb1b7d49`, finished **FAILED**:
dependencies, catalog and regressions PASS; packages failed at the third
consumer, `auth_oidc_agent`; the hosted native-negative phase was SKIPPED.
The failure is test-fixture static-library link ordering, with production
source unchanged. A test CMake fixture fix and fresh rerun remain pending;
there is no complete package or hosted CI PASS.
Earlier hosted failures and their fixes are recorded below. No Gate pin update,
tag or release is claimed.

## Scope

The approved server-side work provides resource-token verification, neutral
credentials for existing sessions, and verified subject propagation through
typed HTTP, stream and WebSocket APIs. Authentication does not confer domain
permissions. Transport callers, external subjects, stored credential bindings
and product principals remain distinct.

This iteration does not implement an identity provider, browser login service,
product authorization, a signer, WASM execution or a second session engine.
Client-side asynchronous token acquisition before outbound requests (F4a) is
part of the original approved plan and this delivery. Generic workload token
exchange (F4b) remains outside the supported first delivery; it requires a
separately qualified STS profile. Existing projected-file acquisition is not
evidence of workload exchange.

## Required Order

1. Qualify the exact backend before adding a dependency or a production adapter.
2. Move existing common credential declarations and validation out of pairing;
   preserve record layouts and existing session mechanics.
3. Implement the asynchronous neutral verifier and the qualified private
   backend with bounded work, finite network limits and safe cancellation.
4. Compose verified subjects into existing trusted invocation and required
   server-supplied injection. Keep transport authentication independent.
5. Integrate existing WebSocket admission rather than creating a second guard;
   reject unauthenticated upgrades before HTTP 101 and enforce finite trust
   lifetime for long-lived calls.
6. Add asynchronous outbound request preparation without Auth imports in the
   HTTP transport, preserving deadlines, cancellation and no mutation replay.
7. Run focused tests, package consumers and independent review cycles. Integrate
   only accepted work into current development and then update the consuming
   product's exact dependency pin.

## Backend Acceptance

The upstream baseline is OpenIDC `liboauth2` 2.4.0 at
`85da4346435aec77f2ff11b529f2c1e8e5033c44`. The qualified fork commit is
`ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5`. Qualification covers the explicit
bounded verification profile with vendor decision caching disabled, not all
upstream configurations or complete application acceptance.

Qualification must establish the following using supported upstream APIs:

- JWT signature, issuer, audience, algorithm, expiry and not-before checks;
  verified token purpose, including different protected `typ` values with the
  same issuer, key and audience.
- Separate opaque-token introspection with resource, subject, expiry and active
  checks. No fallback from a rejected JWT to a different validation mode.
- Bounded response/token/key/claim sizes, finite network timeouts, controlled
  redirects, configured destinations and no token-controlled fetches.
- Distinct credential rejection and backend unavailability without parsing
  error text or leaking token-bearing vendor diagnostics.
- Defined ownership/thread safety, bounded cache lifetime, isolated issuer
  caches and suppression of concurrent unknown-key refreshes.
- Exact dependency provenance and licenses, plus actual platform build/test
  evidence. A source inspection is not a runtime PASS.

If a mandatory capability cannot be provided without duplicating the protocol
engine or relying on private upstream internals, stop backend integration.
Record the evidence and require an upstream fix or a separately approved
backend selection. Do not weaken acceptance to complete the adapter.

## Existing Mechanisms To Reuse

- `forge.auth.oauth2`: client token acquisition and secret ownership.
- `forge.auth.pairing`, `forge.auth.session`, `forge.auth.http`: existing
  credential/session transitions and browser request protections.
- `forge.api.core.trusted_invocation` and `server_supplied`: immutable typed
  context and remote-input replacement; no interceptor decode/re-encode.
- `forge.api.auth.authenticated_caller`: authenticated transport identity;
  never reinterpret it as an end user.
- `forge.api.stream`: existing invocation propagation and stream lifecycle.
- `forge.asio.compute`: bounded blocking work and shutdown; no detached workers
  or blocking network executor.
- Existing HTTP routing and guarded WebSocket implementation: establish its
  commit provenance before integration; keep Auth/API types out of `net.http`.

## Acceptance And Delivery

The feature was integrated locally on a branch based on development commit
`9eac8072575da0ccc1f1337aa456a1f0caecf25c`, not promoted to `dev`.
The historical post-rebase snapshot was rebuilt with LLVM 22.1.8,
Ninja and `-j4` on macOS arm64 and Linux aarch64. Integration removed duplicate
trusted-context declarations while preserving upstream parameter-only DTO
handling, error renderers, buffered WebSocket upgrade input and owned-operation
shutdown. A new regression covers typed admission denial through the upstream
error renderer before payload decoding or service invocation.

The following historical post-rebase results retain their original scope;
in particular, the earlier macOS package PASS is not a final current-source
or Linux installed-package verdict.

| Historical Post-Rebase Gate | Result | Evidence |
| --- | --- | --- |
| macOS verifier, trusted subject, HTTP/WS, preparation, parameters, renderer | 6/6 PASS, 15.58 s | [Regression log](../validation/auth-resource-2026-10-14/rebase-regression-tests.log) |
| macOS credential/pairing/session/HTTP auth, verifier contract, client auth, HTTP plugin | 7/7 PASS, 3.99 s | [Auth log](../validation/auth-resource-2026-10-14/rebase-auth-tests.log) |
| macOS fresh installed Auth package consumers | 6/6 PASS, 82.30 s | [Package log](../validation/auth-resource-2026-10-14/rebase-package-tests.log) |
| Linux auth, trusted subject, preparation, parameters and renderer | 11/11 PASS, 7.85 s | [Auth log](../validation/auth-resource-2026-10-14/linux/rebase-auth-tests.log) |
| macOS structure, network plugin layout and SDK dependency/runtime policy | 4/4 PASS | [Policy log](../validation/auth-resource-2026-10-14/rebase-static-tests.log) |
| Real Keycloak resource acceptance, macOS | PASS | [Resource log](../validation/auth-resource-2026-10-14/real-idp-acceptance.log) |
| Real Keycloak resource acceptance, Linux | PASS | [Resource log](../validation/auth-resource-2026-10-14/linux/real-idp-acceptance.log) |

| Final-Source Gate (2026-10-14) | Result | Evidence |
| --- | --- | --- |
| macOS scoped regressions and policy checks after wait repair | 14/14 CTests PASS, 29.14 s | [Regression log](../validation/auth-resource-2026-10-14/native-final-verified-regressions.log) |
| AppAuth source/P2 regressions, including actual loopback TCP response delivery | 6/6 test cases PASS | [AppAuth log](../validation/auth-resource-2026-10-14/native-appauth-final-tests.log) |
| Linux auth/API regression after both repairs | 11/11 CTests PASS, 7.03 s | [Final rerun](../validation/auth-resource-2026-10-14/linux/final-verified-regressions.log); earlier failed logs retained |
| Full liboauth2 18-case suite, repeated twenty times per platform | PASS: Linux 128.33 s; macOS 112.63 s | [Linux](../validation/auth-resource-2026-10-14/linux/liboauth2-race-repaired-repeat.log), [macOS](../validation/auth-resource-2026-10-14/native-liboauth2-race-repeat.log) |
| Fresh installed macOS API WebSocket consumer | PASS, including corrected auth/HTTP component dependencies | [Package log](../validation/auth-resource-2026-10-14/native-websocket-package-final.log) |
| oidc-agent identical final fixture, pristine baseline | Expected RED: 131 checks, 106 failures, 0 errors | [RED log](../validation/auth-resource-2026-10-14/linux/oidc-expiry-red-final.log) |
| oidc-agent frozen patch, normal and ASan/UBSan | Each 131/131 PASS; P2 CLOSED | [Normal](../validation/auth-resource-2026-10-14/linux/oidc-expiry-green-final.log), [sanitizer](../validation/auth-resource-2026-10-14/linux/oidc-expiry-asan-final.log) |
| macOS actual browser authorization and complete native lifecycle | PASS; current-run user browser attestation; processes exit 0 | [Authorize](../validation/auth-resource-2026-10-14/native-macos-accepted-authorize.jsonl); lifecycle logs below |
| Linux actual patched daemon/library/Forge native lifecycle | PASS, including overrides, expiry renewal and encrypted reload | [Acquire](../validation/auth-resource-2026-10-14/linux/native-oct14/final-acquire-overrides.jsonl); lifecycle logs below |
| Current fixture trust, Keychain, daemon and issuer cleanup | Complete; expected NOT_TRUSTED/absence checks | Confirmed cleanup results below |
| Local full-catalog Linux package build and installed Forge verification | INTERRUPTED at step 491/766 by Docker shutdown; installed consumer PENDING | [Interrupted build](../validation/auth-resource-2026-10-14/linux/package-install-interrupted.log) |
| Hosted Linux catalog and installed consumers at `363423a8` | Run FAILED: dependencies/catalog/regressions PASS; packages FAILED at third consumer `auth_oidc_agent`; native-negative SKIPPED | [Run 38483751720](https://github.com/SystasisIO/forge/actions/runs/38483751720) |

The historical package and SDK policy rows are macOS evidence, not Linux
package/SDK PASS. A historical Linux SDK configure attempt failed because the
toolchain image lacked `libLLVMDemangle.a`; no complete Contract SDK build is
claimed here. This is not an open native-acceptance issue.

The resource fixture uses pinned Keycloak 26.8.0, actual client-credentials
tokens, separate JWT and introspection verifiers, typed unary and server-stream
calls through HTTP and WebSocket admission. A real RSA key rotation preserves
the old public key; the same warmed JWT verifier accepts the new token without
being recreated. Its 600-second snapshot cannot expire during this test. The
driver waits two seconds before requesting rotation, respecting its explicit
one-second unknown-key refresh cooldown; it never retries a rejected token.
Shutdown PASS requires HTTP server stop, verifier drain and compute-pool join.

The Python orchestrator uses Authlib 1.8.0, Requests 2.34.2 and joserfc 1.7.5 in
a private Python 3.12 environment. It changes only a disposable realm whose
ownership marker matches its private generated inputs. Tokens travel through
private pipes, and evidence contains fixed labels only. The WebSocket test peer
uses Beast because the Forge client currently has no custom Upgrade headers;
this proves production server-side admission, not Forge WS client acquisition.
Resource calls use isolated loopback HTTP/WS, while Keycloak endpoints use
verified HTTPS. This is not a production ingress, user SSO or workload-exchange
acceptance claim. See the [driver protocol](../../tests/integration/auth/README.md)
and [issuer fixture](../../tests/auth/live/keycloak/README.md).

Two independent reviewers found the integrated F3 and fixture source scopes
clean after the cooldown correction. The final oidc-agent vendor (including
Makefile/metadata) and adapter scope is CLEAN after the P2 bound fix.
These scoped verdicts and native PASS do not declare whole-delivery CLEAN:
Linux package/installed-consumer validation and final delivery review/ancestry
remain pending. The feature branch was pushed for CI; no promotion to `dev`,
Gate pin update, tag or release has been performed. Generated build evidence
is retained for package validation.

The interrupted package command returned 255 when the Docker API socket
disappeared. Docker's host log records `Quitting whole Docker Desktop` and
`requested backend shutdown`, not a compiler diagnostic. Before shutdown,
four compiler processes were active; the observed slow translation units
were existing Chain API modules. Their slowness was not diagnosed as an SDK
incompatibility or a source failure. This run is not a package PASS, and no
partial-install workaround was used. Free host disk after shutdown: 35 GiB.

### Hosted Linux Verification

The [Auth Resource workflow](../../.github/workflows/auth-resource.yml) runs
only on `auth-api-resource-verification-v1`, using its immutable event SHA.
The [phase script](../../.github/scripts/auth-resource.sh) builds the exact
qualified dependencies, performs the existing full-catalog install, runs
focused regressions and installed consumers, then composes a separate native
driver against the real installed oidc-agent library. Its expected
`setup_required` negative case is package/runtime evidence, not another live
IdP login. The previously completed human login evidence remains separate.

The standard public Ubuntu 26.04 runner uses `-j4` with a two-compiler Ninja
pool, including nested package consumers: four locally observed 5.5 GiB
compiler processes exceed its memory budget. This is a resource limit, not a
smaller source graph. Jobs have finite deadlines and publish allowlisted test
results and phase exit codes; raw logs, tokens and private configuration are
not included in the artifact. Dependencies PASS alone does not establish
catalog, installed-consumer or overall CI PASS.

Reported hosted-run snapshot on 2026-10-14:

| Run | Exact Event SHA | Observed Boundary / Follow-Up |
| --- | --- | --- |
| [38480451537](https://github.com/SystasisIO/forge/actions/runs/38480451537) | `6fed3c48908114a6b239834c1e3229862aa6e0e3` | FAILED in dependency preparation, before Forge: Jansson 2.14.1's CMake minimum/policy compatibility `< 3.5` was rejected by CMake 4. Fixed in `a45747bdc7c276404e8840ee7cc263b5caade884`; this failed run remains historical evidence. |
| [38482513324](https://github.com/SystasisIO/forge/actions/runs/38482513324) | `a45747bdc7c276404e8840ee7cc263b5caade884` | Dependencies PASS; catalog configuration FAILED when dependency `LD_LIBRARY_PATH` shadowed system linker dependencies and system `ld` reported `json_delete`. Fixed in `363423a83bb7f835e95baa0f40a03b3deb1b7d49`; no catalog/package PASS from this run. |
| [38483751720](https://github.com/SystasisIO/forge/actions/runs/38483751720) | `363423a83bb7f835e95baa0f40a03b3deb1b7d49` | Terminal FAILED: dependencies/catalog/regressions PASS; packages FAILED at the third consumer, `auth_oidc_agent`, under GNU ld. The fixture `.pc` placed absolute `stub.a` before `main.o`, leaving `getAgentTokenResponse`, `getAgentTokenResponseForIssuer` and `secFreeAgentResponse` unresolved. Production source unchanged; test CMake fixture fix/rerun pending. Native-negative SKIPPED. |

The first failure is not a Forge compile/test result; the second is a catalog
configuration failure, not completed package acceptance. The third completed
catalog and regressions but failed test-fixture consumer linkage; it is not
an overall package PASS. Keep all three failed attempts separate from their
fixes and any later successful evidence. The skipped hosted native-negative
phase does not change the separately archived human native PASS results.
This snapshot claims no completed hosted CI PASS or fresh human native rerun.

### Historical Post-Rebase Digests

SHA-256 of the historical test driver source, not a current-source manifest:
`6aab0f070332e310f8d880ccb05741f7cb87fc84ed2d8c5e0002118c0d57438a`.

| Artifact | SHA-256 |
| --- | --- |
| macOS six-target regression log | `d872a30e4050ceaf600078e11239ef22c0312c35b090b110f3e5e2057b530bf0` |
| macOS seven-target auth log | `94c0fc13b265dd441e012d1c931f03aacb5a633e9bda66a4cc8700239a12addc` |
| Installed consumer log | `feda75be051447c92d0f8a33cc7c7dff4ab8352376a488e3d23c9867ddf36a75` |
| Linux eleven-target regression log | `5bb571c1fb78380419f948f75011f3dbb67f5aa3e44b0a56ddb4392a26747015` |
| Resource PASS labels, identical on both platforms by design | `afacb304834ee0c406eb9eededfacfd7d16f0637a2e9e9dba188b8ceb8dbe182` |
| macOS Mach-O resource executable | `9dea707e112be01e932966c59b0aec9ca4f252d118d85adf7fd43eb10f783da6` |
| Linux ELF resource executable | `bc80858c1b4a0ffdc8aeea43d1cd9d9e392f21273b72e4b897d5c91af415d49c` |

### Final Native Digests

| Artifact | SHA-256 |
| --- | --- |
| Earlier macOS 14-target native snapshot, before the repairs | `c4ab74d2536d444d155e2b428196cea7913f430a7c8e94a79b0f64f9f9cc243e` |
| AppAuth six-case source/TCP log | `93332c1584f37b23933676db5a53f953542f157a284598cf916506cac71b4323` |
| Current accepted macOS authorize log | `7e37600cd6db5ea19ef0db86adbb7970127d1772ad43b6af74c29e67f022cb1b` |
| Final Linux override refresh log | `b3d44c17748d8a2537b3be61c649de06e5a041b05512156353b4505226ef04c5` |
| Historical failed Linux 11-target rerun, retained | `0f252f9a04081604005d225ccbb92de8bb677198cf6667ef01fc2bfb39caf503` |

Final 131-check RED/GREEN/sanitizer hashes and standalone reproduction commands
are in the [oidc-agent report](../donors/oidc-agent-upstream-report.md).

### Historical Native Attempts

The initial AppAuth attempt did not complete within its 900-second browser
deadline; Linux Device Flow was then NOT_RUN. These were not positive native
acceptance results and are superseded by the fresh final runs below.

In an earlier October 14 attempt, the user completed AppAuth browser authentication
and a fresh real oidc-agent Device Flow. AppAuth authorize exited successfully;
separate-process resume, post-expiry refresh, another resume and local Keychain
invalidate also passed. The issuer observed one authorization-code exchange and
refresh grants increasing from two to five without refresh errors. Safari later
showed a connection error for a loopback callback URL. Its cause has not been
established; that attempt's token lifecycle evidence did not prove correct
browser response delivery and is not recorded as browser UX PASS.

The earlier Linux attempt used unmodified oidc-agent 5.3.8 at
`f31513acaacaa7ebc115b3a329105d449b43ac99`, its actual agent/public C library and
the real Forge provider, not the package-test ABI stub. Human Device Flow
enrollment exited zero. With the explicit configured scopes, acquisition failed:
the returned issuer matched, but the backend's expiry was already in the past,
even though Keycloak recorded a successful refresh. Host and container clocks
agreed. A fresh acquisition reported expiry `1791920193` at time `1791920283`.

Source inspection and an independent review confirmed that this upstream
version updates the account expiry only in its `SAVE_AT` path. Explicit scope
or audience overrides disable that path, while the IPC response still returns
the stored account expiry. The original Forge adapter accepted that stale
positive expiry; its deterministic C ABI test always supplied a fresh one and
did not expose the defect. Neither replacing the expiry nor decoding the token
is an accepted repair.

On October 14 the user approved correcting the backend and created the
`SystasisIO/oidc-agent` fork. Upstream `prerel` at
`431dcd8a930e092a47de87b1782d24ff3727cae0` was checked on that date: the affected
parser, refresh, handler and public token API paths were unchanged relative
to `v5.3.8`. This historical comparison is not a claim about every current
upstream release. The resulting patch and final evidence are recorded below.

The historical diagnostic run with `--scopes ''` used the enrolled account's existing scope
profile, not anonymous access or a token without permissions. Acquire, separate
resume, actual-expiry refresh and a further process resume passed. Issuer
Device Flow exchanges were one; refresh grants increased from three to four
during the measured refresh phase, with zero errors. After account unload, the
same Forge driver returned typed `setup_required` and exit 3 as required.
These results do not qualify explicit scope/audience overrides or prove a
particular token audience. The resource verifier remains responsible for
validating the recipient and effective claims.

The first post-P2 rebuild/install passed, but its existing fixture refresh token
was inactive and required reenrollment. Fixture expiry was a possible, unconfirmed
cause. That attempt is not post-fix live PASS; its two refresh errors predate the
different fixture/run ID used for the final cycle. The initial sanitizer link
attempt lacked compiler runtime libraries and was NOT PASS, unlike the final run.

### Current Native Acceptance

The recorded native cycles below ran at Forge revision
`d5d29aac774eecd06a39371459cac5c5099c6c85`. The separately reviewed runtime
revision `b099d6aaca417c06bb4922abadf604cbbcfde638` includes both repairs and
the passing regression reruns recorded below. Published feature/workflow
revision `363423a83bb7f835e95baa0f40a03b3deb1b7d49` is a separate hosted-CI
revision, not the SHA of these recorded human native cycles.
Parent pin commit
`813735bc21e1030daaa57fa29d0918bd3bf7ba31` records the published oidc-agent
[28d6f89701b64d5150b05d01f85b8a8eed54808e](https://github.com/SystasisIO/oidc-agent/commit/28d6f89701b64d5150b05d01f85b8a8eed54808e),
profile `5.3.8-systasis.1`. It carries each returned token's own expiry without
overwriting default-cache lifetime. Forge rejects unknown, unrepresentable,
expired or insufficiently valid expiry. Full scopes/audience/minimum validity
are preserved, with no stripped overrides, fabricated expiry, JWT-decoding or
alternate-provider fallback. The actual serving daemon, including forwarded
routes, must be patched; updating only its C client library is insufficient.

Linux final native cycle: **PASS** with the actual rebuilt/installed post-P2
daemon, new library and Forge binary at that exact fork pin. Fresh human-confirmed
Device Flow enrollment and `oidc-gen` exited 0. Scope + audience acquisition
and renewal after actual expiry, audience-only resume, and daemon restart plus
encrypted-account load (exit 0) and resume all passed:
[acquire](../validation/auth-resource-2026-10-14/linux/native-oct14/final-acquire-overrides.jsonl),
[refresh](../validation/auth-resource-2026-10-14/linux/native-oct14/final-refresh-overrides.jsonl),
[resume](../validation/auth-resource-2026-10-14/linux/native-oct14/final-resume-audience.jsonl),
[restarted resume](../validation/auth-resource-2026-10-14/linux/native-oct14/final-restarted-resume.jsonl).
The exercised client's measured refresh count increased
[14 before](../validation/auth-resource-2026-10-14/linux/native-oct14/final-counts-before-refresh.jsonl)
to [16 after](../validation/auth-resource-2026-10-14/linux/native-oct14/final-counts-after-refresh.jsonl),
with `refresh_error` **2 -> 2: no new errors, not a zero cumulative total**.
After intentional account removal (oidc-add exit 0), the
[unload control](../validation/auth-resource-2026-10-14/linux/native-oct14/final-after-unload.jsonl)
returned typed `setup_required`, JSON `BLOCKED` and driver exit 3 as expected.
This is a passing negative control, not an open native failure. These live tests
do not prove Keycloak honored resource/audience parameters; controlled boundary
fixtures prove wire overrides, and the resource-server verifier enforces recipient.

macOS final native cycle: **PASS**. The current-run
[authorize](../validation/auth-resource-2026-10-14/native-macos-accepted-authorize.jsonl) log
records success, and the user confirmed the browser displayed
"Authorization complete. Return to the app." for this exact run.
[Resume 1](../validation/auth-resource-2026-10-14/native-macos-final-resume-1.jsonl),
[post-expiry refresh](../validation/auth-resource-2026-10-14/native-macos-final-refresh.jsonl),
[resume 2](../validation/auth-resource-2026-10-14/native-macos-final-resume-2.jsonl) and
[invalidate](../validation/auth-resource-2026-10-14/native-macos-final-invalidate.jsonl)
all PASS with processes exiting 0; invalidation confirms local state absent.
Desktop counts [before](../validation/auth-resource-2026-10-14/native-macos-final-counts-before.jsonl)
and [after](../validation/auth-resource-2026-10-14/native-macos-final-counts-after.jsonl) show
authorization-code exchanges **2 -> 2**, refresh **6 -> 9**, errors **0 -> 0**.
The earlier Safari error is historical; no current browser action remains pending.
The 14/14 macOS regressions and six AppAuth source/TCP cases also PASS.

### Linux Regression Follow-Up

The historical failed [Linux regression log](../validation/auth-resource-2026-10-14/linux/final-regressions.log)
records **10/11 CTests PASS, one failed target**, not the current passing rerun.
`test_forge_api_trusted_subject` ran its 21-case source suite and reported two
fatal backpressure precondition failures: `before.queued_writes > 0U` and
`stalled.sent_messages == before.sent_messages`. Commit `1f9fddc6` replaces
fixed setup sleeps with bounded observation of existing connection metrics.
Pending-write, cancellation, queue cleanup and join assertions remain intact.
Twenty consecutive repeats pass on both
[Linux](../validation/auth-resource-2026-10-14/linux/websocket-observation-repeat.log)
and [macOS](../validation/auth-resource-2026-10-14/native-websocket-observation-repeat.log).

A separate liboauth2 verification hang appeared in the broader rerun and was
reproduced under a verbose [180-second timeout](../validation/auth-resource-2026-10-14/linux/liboauth2-repeat-diagnostic.log).
The [live stack](../validation/auth-resource-2026-10-14/linux/liboauth2-hang-stack.log)
shows `fixture::verify` waiting for its result while compute and I/O workers
are idle, not fixture teardown or an active blocking HTTP call. Source review
identified concurrent re-arming and cancellation of the structured notification
race on a bare multithread executor. The stack supports, but cannot itself prove,
the exact interleaving. Commit `b099d6aa` runs the complete notification race on
one per-invocation strand, without serializing physical compute across calls.
The unchanged bounded stress test is [RED](../validation/auth-resource-2026-10-14/linux/liboauth2-race-red.log)
at round 22 before the fix (5.13 s, exit 201), then
[GREEN](../validation/auth-resource-2026-10-14/linux/liboauth2-race-green.log)
for all 8192 calls after it (0.906 s, exit 0). Full-suite repeats and the final
broader regressions pass on both platforms as recorded above. The wait regression
is closed; earlier failures are retained rather than recategorized as PASS.

### Remaining Delivery Gates

User-mediated native gates are closed; service-account/resource tests have not
been substituted for them. The local full-catalog Linux package run is
**INTERRUPTED**, not running. Hosted run `38483751720` is terminal FAILED:
dependencies/catalog/regressions PASS, packages FAILED at the `auth_oidc_agent`
test consumer, native-negative SKIPPED. Repair the test CMake fixture's static
archive link ordering, then obtain a fresh passing package and native-negative
rerun. Production source is unchanged by this fixture issue. Runtime regressions
are repaired and their failed evidence retained. Complete hosted Linux validation and
record its results, then obtain final delivery review
and refresh development ancestry before any authorized promotion or pin update.
Remove retained build resources only after their validation work is finished.
Selected sanitized acceptance and regression evidence is retained under
`docs/validation`, outside generated builds. Final Linux package evidence must
join it before cleanup.
Native PASS and scoped CLEAN are not overall delivery CLEAN or permission to
promote to `dev` or update Gate. The authorized feature-branch push is separate.

### Historical Interrupted Cleanup

The isolated AppAuth Keychain item was invalidated. The `forge-auth-live`
Compose project and its data volume, and the `forge-auth-resource-linux`
container with its disposable agent, were removed. Generated fixture passwords,
client credentials, realm import and TLS private key were deleted. Other
projects' containers, volumes and cached images were not changed.

The earlier temporary user-level localhost trust and exact Keychain certificate
were removed, and certificate verification confirmed it was no longer trusted.
Its SHA-256 was
`F15ECE0A65DF5E459537D9F7E28519B07778A17E836972991D62F2B27AAD5568`.
These historical cleanup facts do not describe the current retry's certificate.

### Current Fixture Cleanup

The October 14 retry's explicitly approved localhost-only leaf certificate was
`E93117B9EFC93A3D27C51BFF0D8814DFB03D3FBFB3253FD9896380BD8423554D`.
Its user-confirmed `security remove-trusted-cert` and exact-SHA certificate
deletion both exited 0. SSL localhost verification then returned
`CSSMERR_TP_NOT_TRUSTED` (exit 1, expected); certificate lookup returned absence
(exit 44). The isolated AppAuth Keychain item is also absent (exit 44).
The actual owned daemon processes (PIDs 10661/10667) are stopped. Removal of the
owned `forge-auth-live` Compose container, data and network is confirmed complete.
These results were checked by the coordinator after the user confirmed the
macOS trust-removal prompt.

Retained local Linux build resources belong to the **INTERRUPTED** package run,
not a running local build. Their final cleanup is pending after package validation;
this does not leave the current localhost certificate or native fixture active.

Credential migration is a scoped public source break, not a wire/storage reset.
Document the mechanical consumer migration and preserve enum and field order
with legacy-writer fixtures. Do not retain old namespace aliases.

Required coverage includes credential/session regressions; verifier negative
cases, key rotation, limits and cancellation; spoofed unary/stream-open fields;
separate TLS and user identity; pre-upgrade rejection; concurrent-user context
isolation; expiry/revocation and shutdown; and installed package boundaries.

Use one coordinator-owned generated build at a time with Homebrew LLVM, Ninja
and `-j4`. Record platform tests that were not run as `NOT_RUN`. Do not delete
other tasks' builds, sources or data. Remove this iteration's generated build
after validation and the authorized integration into `dev`.

The final delivery requires two independent clean review verdicts and a fresh
development ancestry check. No force push, release/tag or unrelated product
changes. A failed mandatory qualification is not grounds to push a partial
authentication path or update the consuming product's pin.
