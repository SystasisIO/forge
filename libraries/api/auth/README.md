# forge.api.auth

`forge.api.auth` carries a caller identity established by a server-side transport
binding. It is not an identity provider, session store, authorization engine, or
transport listener.

## Public Module

- `forge.api.auth.authenticated_caller`
- `forge.api.auth.authenticated_subject`

## Resource Subject

`authenticated_subject` owns one immutable `forge::auth::oauth2::verified_token`.
The verifier, not request JSON, headers, email or a TLS peer, establishes it.
`token()` returns a const pointer; a default/decoded placeholder has no token.
Its Raw representation is empty and JSON representation is `{}`. Verified claims
and the private `authenticated()` marker are never serialized or reflected.

```cpp
import forge.api.auth.authenticated_subject;
import forge.api.core.invocation_context;

auto verified = co_await verifier.async_verify(std::move(secret), route_policy);
auto context = forge::api::core::invocation_context{
   .trusted = forge::api::core::trusted_invocation_builder{}
      .set(forge::api::auth::authenticated_subject{std::move(verified)})
      .build(),
};
```

The required `server_supplied` specialization clears client input and fails if
the trusted subject is missing, empty or no longer valid. Only successful server
application sets `authenticated()`. Transport caller and subject remain separate
types; neither establishes product permissions. Bindings bound stream lifetime
by `verified_token::valid_until()` even if the provider requested a later expiry.
Add a bounded recheck hook for earlier revocation; offline evidence alone cannot
promise immediate revoke. Providers must not capture request references or raw
credentials in that hook. Focused doubles live in
[`trusted_subject_tests.cpp`](../../../tests/http_websocket/trusted_subject_tests.cpp);
they do not prove real issuer verification.

## Transport Boundary

`authenticated_caller` is a `forge::api::core::server_supplied` value. A remote
client can serialize the field only as an ignored placeholder: the server resets
it before handler dispatch and replaces it from `trusted_invocation`. A binding
must add the value only after it has authenticated the transport principal.

```cpp
import forge.api.auth.authenticated_caller;
import forge.api.core.trusted_invocation;

auto trusted = forge::api::core::trusted_invocation_builder{}
   .set(forge::api::auth::authenticated_caller{
      forge::api::auth::caller_source::tls_certificate,
      certificate_fingerprint,
   })
   .build();
```

Consumers compare the typed source and certificate/peer fingerprint using their
own exact authorization policy. Do not infer authorization from the existence
of an authenticated caller, accept client-supplied identity material, or place
credentials in this value.

## Target

`forge_api_auth`
