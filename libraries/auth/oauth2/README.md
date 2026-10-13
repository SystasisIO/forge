# Forge Auth OAuth 2

`forge_auth_oauth2` provides a narrow, transport-neutral access-token boundary.
It owns a move-only `access_token`, request constraints and the asynchronous
`token_provider` / `interactive_authorizer` contracts. A provider owns its
refresh state; no refresh token, authorization code, browser redirect, PKCE
exchange or OAuth state machine is part of this library.

Resource servers use the separate `token_verifier` contract. A provider acquires
a bearer; a verifier checks a presented bearer and returns immutable evidence.
Neither operation creates a durable credential binding or product permissions.

## Modules

- `forge.auth.oauth2.exceptions`: typed configuration, setup and token failures.
- `forge.auth.oauth2.access_token`: move-only access-token value.
- `forge.auth.oauth2.token_provider`: request records and asynchronous
  token-provider interface.
- `forge.auth.oauth2.interactive_authorizer`: explicit interactive lifecycle
  contract for authorize, revoke and invalidate operations.
- `forge.auth.oauth2.verification_policy`: trusted issuer/resource/version,
  explicit JWT or opaque introspection profile, algorithm allowlist, required
  scopes, clock skew and finite freshness budget.
- `forge.auth.oauth2.token_verifier:verified_token`: exported value partition
  (consumers import `forge.auth.oauth2.token_verifier`), normalized read-only issuer, subject,
  audiences, scopes, expiry and verification/freshness evidence; optional
  `client_id` is preserved only when actually verified by the backend.
- `forge.auth.oauth2.token_verifier`: asynchronous verification and explicit
  admission-stop/drain contract for backend implementations.

The value partition and verifier share one named-module owner so that the
private constructor/friend boundary remains valid C++ modules. Do not import
the partition directly or introduce a separate public construction module.

```cpp
import forge.auth.oauth2.token_provider;

boost::asio::awaitable<void> use(forge::auth::oauth2::token_provider& provider) {
   auto token = co_await provider.async_access_token({
       .scopes = {"storage.read"},
       .minimum_validity = std::chrono::seconds{60},
   });
   consume_bearer(token.secret().view());
}
```

`access_token` cannot be copied. Its `secret_string` is intentionally not
described, serialized or logged by this leaf. Provider implementations must not
put token material in exception context. `exceptions::setup_required` is the
typed signal that an interactive or administrator-controlled setup step is
needed.

Dependencies: `forge_crypto_core`, `forge_exceptions` and Boost.Asio headers.

## Resource Admission

```cpp
import forge.auth.oauth2.token_verifier;

boost::asio::awaitable<void> admit(
    forge::auth::oauth2::token_verifier& verifier,
    forge::crypto::core::secret_string bearer) {
   const auto evidence = co_await verifier.async_verify(std::move(bearer), {
       .issuer = "https://issuer.example",
       .audience = "resource-api",
       .version = "v1",
       .algorithms = {forge::auth::oauth2::signature_algorithm::rs256},
       .required_scopes = {"read"},
       .freshness = std::chrono::seconds{60},
   });
   if (!evidence.valid_at(std::chrono::system_clock::now())) {
      throw forge::auth::oauth2::exceptions::invalid_token{"authentication evidence expired"};
   }
   // Resolve issuer + subject through the service's independently owned binding policy.
}
```

The policy above is administrator-controlled route configuration. Do not derive
it from request JSON, an unverified `iss`, or a token-supplied endpoint. The
neutral interface owns no endpoints, discovery, HTTP, JWT parser or signature
implementation. Concrete verifiers must reject profiles they have not configured.
JWT failure never switches to introspection; this JWT profile requires RFC 9068
protected `typ`, not an ID token with a matching signature and audience.

`verified_token` has no public/default constructor, assignment, raw bearer,
arbitrary JSON claims map or reflection/serialization API. Only a trusted
`token_verifier` implementation can call its protected `make_verified_token`
factory. Implementing that interface is a trust boundary: the factory checks
structural invariants but does not perform cryptographic verification. Test
doubles are not production validators. Consumers compose the result rather
than copying its fields into a second identity record.

`valid_until()` is the freshness deadline, never later than `expires_at()`;
`valid_at(now)` accepts exactly `[verified_at(), valid_until())`. There is no
implicit global clock. A moved-from value always has an empty validity interval
and cannot authorize admission. Session/stream admission must enforce this deadline and
any independent binding/revocation freshness, not just the original JWT expiry.
Optional absent claims are not invented. Audiences and scopes are canonical
sorted sets; an empty scope grant is valid evidence, not permission to act.

Implementations close admission with `request_stop()` and await `async_drain()`
before destroying runtime/vendor dependencies. A canceled caller never receives
a late result; synchronous vendor work must run on bounded `forge.asio.compute`
with finite upstream timeouts. No detached worker or executor-blocking destructor
is an acceptable replacement for that lifecycle. Keep the implementation alive
until submitted coroutines and drain complete.

`invalid_token`, `policy_denied`, `issuer_unavailable`, `input_too_large`,
`response_too_large`, `busy`, `stopped` and `canceled` are distinct typed failures.
Do not map an issuer outage to an invalid bearer/HTTP 401, classify exception
text, or forward vendor messages/claims to logs.

## Tests

The repository keeps executable auth tests in the central `tests/` target, so
this leaf does not create an unreachable local test target. Its integration
must add central unit coverage for move-only token ownership, typed
`setup_required`, lifecycle operations, and a provider that retains refresh
state privately.

`tests/auth/token_verifier_tests.cpp` covers construction/access restrictions,
immutable ownership, canonical sets, finite policy, disjoint profiles, typed
errors and exact freshness boundaries with explicit clocks. These are neutral
contract tests, not JWT, network, backend qualification or live IdP acceptance.

This library supplies verification contracts, not an OAuth/JWT engine,
credential store or HTTP transport. Use a separately qualified backend to
implement the resource-server interface.
