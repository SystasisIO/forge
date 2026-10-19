# Auth API

Stable, native-only contract leaf. Target `forge_auth_api`, package component
`auth_api`. It depends on API Core/Auth, Auth Attestation, Raw, Variant and
existing typed exception machinery. It has no JOSE/backend, Chain, custody,
signer plugin or runtime implementation dependency.

## Attestation Signer 1.0

Import `forge.auth.api.attestation_signer`. The local/remote contract identity
is `forge.auth.api.attestation_signer`, major 1, revision 0, exported through
the existing `FORGE_EXPORT_API` machinery:

```cpp
boost::asio::awaitable<forge::auth::attestation::signed_document>
sign(forge::auth::attestation::document<forge::variant_object> request,
     forge::api::auth::authenticated_caller caller);
```

There is one typed request model, instantiated from the shared document
template. No arbitrary bytes/digest signing method or caller-selected key,
algorithm or profile is exposed. Document, product claims and API versions
are independent.

`authenticated_caller` retains its existing required server-supplied
semantics. Client bytes cannot establish identity: the transport injects the
trusted caller and marks it authenticated. Missing trusted input fails before
service invocation. Direct typed local calls are not implicit authorization.

The service implementation must enforce an explicitly allowed local/remote
profile, select exactly one matching profile, authenticate its backend result
and redact vendor errors. Those runtime decisions belong to the later signer
plugin implementation, not this F1 contract. No default-allow stub is shipped.

`forge.auth.api.exceptions` reuses common invalid-document/profile types and
declares typed admission-denied, ambiguous-profile, key-unavailable,
invalid-backend-signature, busy, canceled and stopped errors with existing API
descriptor projection/restoration. Common profile validation failures are
declared as well. Never include a credential, private material or vendor error
text in an API error.

`signed_document` owns only the Compact string. Parsed fields, signature
authenticity and product admission must be derived from the signed bytes; no
second authoritative copy is carried in the response. Protected transport and
an explicitly agreed credential-size limit remain binding requirements.

## Validation

`test_forge_api_attestation` uses `tests/unit/main.cpp` plus
`tests/test_api_attestation.cpp`, without another main. Tests use a recording
contract mock, not an implementation of signing: descriptors/version, local
binding, spoofed/missing caller, generated remote proxy, Raw response and typed
error projection. Crypto, installed-package and plugin admission acceptance
remain separate gates.
