# Auth/API Resource Verification

Date: 2026-10-13.
Status: server implementation and resource acceptance passed; native interactive acceptance pending.
Branch: `auth-api-resource-verification-v1`.
Starting development commit: `411644237490a63a52d1f810336673367c962651`.

## Current Outcome

The exact liboauth2 candidate fails mandatory error-classification and redirect
containment requirements. Source findings were independently confirmed by two
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
test manifest for the fork. No partial authentication implementation has been
pushed to `dev`, and Gate's Forge pin has not been changed.

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

The feature was integrated onto development commit
`9eac8072575da0ccc1f1337aa456a1f0caecf25c` and rebuilt with LLVM 22.1.8,
Ninja and `-j4` on macOS arm64 and Linux aarch64. Integration removed duplicate
trusted-context declarations while preserving upstream parameter-only DTO
handling, error renderers, buffered WebSocket upgrade input and owned-operation
shutdown. A new regression covers typed admission denial through the upstream
error renderer before payload decoding or service invocation.

| Post-integration gate | Result | Evidence |
| --- | --- | --- |
| macOS verifier, trusted subject, HTTP/WS, preparation, parameters, renderer | 6/6 PASS, 15.58 s | `build-auth-resource/rebase-regression-tests.log` |
| macOS credential/pairing/session/HTTP auth, verifier contract, client auth, HTTP plugin | 7/7 PASS, 3.99 s | `build-auth-resource/rebase-auth-tests.log` |
| macOS fresh installed Auth package consumers | 6/6 PASS, 82.30 s | `build-auth-resource/rebase-package-tests.log` |
| Linux auth, trusted subject, preparation, parameters and renderer | 11/11 PASS, 7.85 s | `build-auth-resource/linux/rebase-auth-tests.log` |
| macOS structure, network plugin layout and SDK dependency/runtime policy | 4/4 PASS | `build-auth-resource/rebase-static-tests.log` |
| Real Keycloak resource acceptance, macOS | PASS | `build-auth-resource/real-idp-acceptance.log` |
| Real Keycloak resource acceptance, Linux | PASS | `build-auth-resource/linux/real-idp-acceptance.log` |
| AppAuth browser login, Keychain restart and refresh | BLOCKED: login was not completed within the 900 s browser deadline | Not a positive native acceptance result |
| Real oidc-agent Device Flow, process restart and refresh | NOT_RUN: disposable agent prepared, human enrollment not completed | Setup-required probes do not prove successful enrollment |

The package and SDK policy rows are macOS evidence, not Linux package/SDK PASS.
A separate Linux SDK configure attempt was blocked by the toolchain image's
missing `libLLVMDemangle.a`; no complete Contract SDK build is claimed here.

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
clean after the cooldown correction. Whole-delivery CLEAN and push remain
blocked on mandatory native interactive acceptance. No Forge development push,
Gate pin update, tag or release has been performed. Generated build evidence is
retained for the remaining acceptance and removed only after accepted integration.

### Evidence Digests

SHA-256 of the exact test driver source:
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

### Remaining Native Gate

Use only the disposable users and public native clients from the fixture.
Complete AppAuth Authorization Code + PKCE in the system browser, then run
resume, post-expiry refresh, a second process resume and local invalidate.
Enroll the real Linux agent with Device Flow from a separate browser, then run
acquire, process resume, post-expiry refresh, unload and setup-required checks.
For both providers, safe issuer refresh-grant counts must increase. Remove the
isolated Keychain item, agent, fixture container/data and the exact temporary
localhost certificate trust after testing, including interrupted attempts.
Do not substitute service-account tokens or the passing resource fixture for
these user-mediated flows. Refresh development ancestry before final delivery.

### Interrupted Fixture Cleanup

The isolated AppAuth Keychain item was invalidated. The `forge-auth-live`
Compose project and its data volume, and the `forge-auth-resource-linux`
container with its disposable agent, were removed. Generated fixture passwords,
client credentials, realm import and TLS private key were deleted. Other
projects' containers, volumes and cached images were not changed.

Temporary user-level localhost certificate trust removal is still pending a
macOS authorization dialog; it is not a completed cleanup gate. Retain the
public certificate until the pending command finishes, then delete only its
exact Keychain certificate and confirm it is no longer trusted. Its SHA-256 is
`F15ECE0A65DF5E459537D9F7E28519B07778A17E836972991D62F2B27AAD5568`.
Recreate disposable credentials and an explicitly trusted test certificate for
the next native attempt; do not reuse the interrupted fixture's secrets.

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
