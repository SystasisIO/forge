# Auth/API Resource Verification

Date: 2026-10-13.
Status: blocked at F0 qualification; no resource-token verifier is delivered.
Branch: `auth-api-resource-verification-v1`.
Starting development commit: `411644237490a63a52d1f810336673367c962651`.

## Current Outcome

The exact liboauth2 candidate fails mandatory error-classification and redirect
containment requirements. Source findings were independently confirmed by two
Sol reviewers and reproduced by eight standalone macOS cases. See the
[qualification report](../donors/auth-liboauth2-qualification.md) for exact
dependency provenance, observed results and remaining NOT_RUN coverage.

Only the execution note, qualification report and standalone test fixture have
been added. Production Forge sources, vendor pins and package exports remain
unchanged. No partial authentication implementation has been pushed to `dev`,
and Gate's Forge pin has not been changed. The next decision is an upstream
dependency fix or a separately approved backend candidate, followed by F0
qualification again. Neither choice has been silently substituted for the plan.

## Scope

The approved server-side work provides resource-token verification, neutral
credentials for existing sessions, and verified subject propagation through
typed HTTP, stream and WebSocket APIs. Authentication does not confer domain
permissions. Transport callers, external subjects, stored credential bindings
and product principals remain distinct.

This iteration does not implement an identity provider, browser login service,
product authorization, a signer, WASM execution or a second session engine.
Client-side asynchronous token acquisition before outbound requests is a
separate follow-up, not a claim made by this server-side delivery.

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
6. Run focused tests, package consumers and independent review cycles. Integrate
   only accepted work into current development and then update the consuming
   product's exact dependency pin.

## Backend Acceptance

The candidate is OpenIDC `liboauth2` 2.4.0 at
`85da4346435aec77f2ff11b529f2c1e8e5033c44`. This is a qualification input,
not an approved vendor pin.

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
after validation.

The final delivery requires two independent clean review verdicts and a fresh
development ancestry check. No force push, release/tag or unrelated product
changes. A failed mandatory qualification is not grounds to push a partial
authentication path or update the consuming product's pin.
