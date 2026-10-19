# Auth Attestation

Stable, product-neutral attestation values and pure profile validation. An
attestation is a signed confirmation, not hardware attestation, OAuth identity,
or permission to perform a product operation. No signer/verifier runtime,
custody, network, Chain, scheduler, or plugin belongs to this leaf.

## Modules And Targets

Target `forge_auth_attestation`, package component `auth_attestation`:

- `forge.auth.attestation.document`: one physical `document<Claims>` and its
  `extension<Claims>`; ordered Boost.Describe metadata is the Raw field registry.
- `forge.auth.attestation.verification_policy`: exact expected identifiers,
  lifetime/skew policy, format bounds and pure identifier checks.
- `forge.auth.attestation.signed_document`: one owning Compact credential value,
  Raw customization and diagnostic redaction.
- `forge.auth.attestation.exceptions`: typed native errors and guest assertion
  tags through the existing exception policy.
- `forge.auth.attestation.document_schema`: native-only Schema rules, the
  `document_schema<Claims>()` artifact exporter, and deliberate signed-value
  wire conversion. Reuses `forge.schema` and `forge.variant.schema`.

The native target uses `forge_add_contract_library` with shared declarations and
implementations. Source selection retains the guest-safe boundary: a future
guest build excludes schema, variant, JSON, RTTI, crypto, Asio and native
exception modules. Native builds also select `document_schema`; they do not
depend on `FORGE_ENABLE_JOSE`.

This delivery qualifies the native target only. Guest SDK registration,
artifacts and ABI/Raw execution qualification are deferred until the WAMR
delivery from R&D; this leaf does not claim a qualified guest SDK component.

## Validate An Authenticated Document

The verifier first authenticates the original Compact bytes through the JOSE
owner. Decode its authenticated JSON using the existing strict JSON codec,
with all format bounds from this leaf, before calling `validate`:

```cpp
#include <chrono>

import forge.auth.attestation.document;

auto expected = forge::auth::attestation::verification_policy{
   .issuer = "urn:example:issuer",
   .audience = "urn:example:resource",
   .purpose = "example.read",
   .schema = "urn:example:read:v1",
   .max_lifetime = std::chrono::seconds{60},
   .clock_skew = std::chrono::seconds{5},
};
// authenticated_document has type document<product_claims>.
authenticated_document.validate(expected, trusted_now);
```

Never derive `expected` from an untrusted document. Issuer, audience and schema
are bounded ASCII URI identifiers compared byte-for-byte, with no
normalization, wildcard matching, dereference or network lookup. The shared URI
check currently enforces scheme, URI characters and percent encoding, not the
complete authority/IPv6 grammar; full URI syntax qualification remains an
explicit integration gap, not a claimed gate. Purpose is
`[a-z][a-z0-9._-]*`; `jti` is `g_` plus 64 lowercase hex digits. The issuance
host generates the ID with the existing crypto RNG and retains ID/timestamps
across retries; this shared leaf introduces no guest randomness.

`iat`, `nbf`, `exp` are `int64_t` Unix seconds in `0..253402300799`, with
`iat <= nbf < exp`. Positive configured `max_lifetime` and nonnegative
`clock_skew` are mandatory. Skew permits bounded future issuance/activation,
but `now >= exp` always fails. Checks never mutate the document. There is no
implicit wall clock, backdating rewrite, replay ledger or admission decision.

## Native Schema And Wire Boundaries

Import `forge.auth.attestation.document_schema` before instantiating native
schema/JSON consumers. The generic signing request is exactly
`document<forge::variant_object>`; typed consumers instantiate the same
template with a described product claims record. Product Schema rules own
unknown fields, decimal uint64 counter strings and their numerical bounds.
The native exporter delegates to the existing constraint exporter, without a
second authoritative field registry or product ABI decoder.

The JSON consumer must select exact described-record decoding, reject unknown
fields, and apply the declared depth/node/container/string/payload limits.
Schema checks do not replace profile/time checks or product checks against the
actual verified key, trusted snapshot, scope, revocation and current request.

`signed_document` stores only Compact. Construction and Raw/variant decoding
do not establish signature authenticity. It has no implicit string conversion
or parsed payload copy; `compact()` explicitly exposes the credential for
intentional wire transport or protected storage. It checks the byte boundary;
the JOSE owner alone parses/validates JWS segments and signatures. A default
value is only a decode destination, not a valid signed response.

`diagnostic_is_secret` redacts signed values, including nested diagnostic
traversal. Never pass `compact()` to logs, config, exception text, URLs or
telemetry. Deliberate Raw/variant wire serialization is not diagnostic
serialization. The API signer contract lives separately in `auth/api`.

## Validation

`test_forge_auth_attestation` uses `tests/unit/main.cpp` plus
`tests/test_auth_attestation.cpp`. It covers field/Raw layout, exact profile,
timestamp extremes and nonextended expiry, native nested dynamic claims,
product decimal bounds, schema export and credential redaction. Native installed
consumers are a separate CI gate. Guest ABI/Raw parity remains deferred to WAMR,
not implied by the native tests.
