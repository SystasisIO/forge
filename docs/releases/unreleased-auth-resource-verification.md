# Unreleased Auth Resource Verification

This change is under development and has no release number or tag. Runtime,
package and native-login acceptance are recorded separately in the
[iteration report](../iterations/auth-api-resource-verification-v1.md).

## Source And Package Migration

The explicitly approved credential relocation is a C++ source/package break.
Common credential IDs, bindings, records and validation now belong to
`forge.auth.credential`, not Pairing. Session and HTTP policy consume that
neutral owner. Pairing still owns bootstrap, pending approval and one-time
exchange. There are no compatibility aliases or duplicated records.

Existing credential/session/device field order, enum values and Raw layouts
are preserved. Do not reset consumer databases. Rebuild affected module
consumers and apply the exact symbol, exception and component mapping in the
[migration guide](../migration/auth-credential-v1.md). That guide distinguishes
Forge byte-compatibility fixtures from consumer-owned full database reopen
tests. This change must not be shipped in a patch release.

## Added Surfaces

- `auth_oauth2` provides neutral resource-token verification contracts in
  addition to existing client-side acquisition. Verification is not product
  permission or provisioning.
- The optional `auth_liboauth2` component uses a pinned maintained liboauth2
  fork with bounded transport controls and typed failures. CJOSE, Jansson and
  libcurl remain the JOSE/JSON/HTTP engines; Forge does not implement another
  OAuth or JWT engine. The Contract SDK excludes this host backend.
- API bindings propagate a checked subject separately from the existing
  transport caller through trusted invocation and required server-supplied
  fields. HTTP/stream/WebSocket lifetime enforcement does not extend an
  expired authentication decision.
- HTTP client request preparation supports asynchronous acquisition within
  the request deadline, before sending bytes. Cancellation or late acquisition
  must not send a request; uncertain mutations are not replayed automatically.

The maintained backend qualifies an explicitly bounded profile only. Forge
disables vendor authentication-result caching unconditionally and rejects
redirects. Legacy/default vendor configurations are not supported profiles.
Offline signed-token validation is not a revocation feed; the application
still owns current bindings, finite freshness and product authorization.

No Gate, Storlane or Turbine business policy, IdP service, signer API or WASM
runtime is introduced by these libraries. Native AppAuth/oidc-agent login and
external product integration require their own executed acceptance evidence.
