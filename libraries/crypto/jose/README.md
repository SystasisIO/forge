# JOSE

Public-key-only ES256 JWS Compact preparation, completion and local verification
over the qualified CJOSE external-signing API. Use this leaf when a caller owns
trusted public keys and an exact protected-header policy. It does not implement
JWT claims, authorization, key custody, provider orchestration or network lookup.

Target: `forge_crypto_jose`. Package component: `crypto_jose`. Host-only, enabled
by `FORGE_ENABLE_JOSE`; it does not require enabling OAuth.

## Public Modules

- `forge.crypto.jose.header_policy`: exact type/key-ID requirements and reducible
  Compact/header/payload byte limits.
- `forge.crypto.jose.trusted_keys`: public key inputs, cryptographic key bindings
  and owning immutable trust snapshots.
- `forge.crypto.jose.codec`: `prepare`, move-only `prepared_message`, `verifier`
  and the non-input-constructible `verified_payload` result.
- `forge.crypto.jose.exceptions`: typed, redacted boundary errors.

```cpp
#include <string_view>

import forge.crypto.jose.codec;

namespace jose = forge::crypto::jose;

jose::verified_payload verify_document(
    std::string_view compact,
    forge::crypto::asymmetric::p256::public_key_point_data operator_public_key) {
   const auto policy = jose::header_policy{.typ = "application/example+jws"};
   const auto keys = jose::trusted_keys{
       {.trust_domain = "example-deployment", .verifier_profile = "example-document-v1", .revision = 1},
       {{.kid = "operator-key", .public_key = operator_public_key}}, policy};
   return jose::verifier{}.verify(compact, keys);
}
```

`verified_payload` owns exact authenticated bytes and retains the snapshot after
the caller releases or replaces its handle. `key_binding()` selects the actual
verified snapshot entry: its canonical `asymmetric::r1_public_key` and
cryptographic `identity`, not merely the untrusted `kid` string.
`trust_snapshot().identity()` hashes canonical Raw-framed domain, profile,
revision, sorted key mappings, exact header policy and all byte limits. Equal
numeric revisions in different domains/profiles/keysets are not equal identities.
Key order alone does not change identity. No verification cache is provided.

The application still owns snapshot freshness, issuer/audience/claim validation,
expiry, delegation, revocation and request scope. Successful JOSE verification
does not imply access. Consumers retaining verification results must compare the
entire current snapshot identity and recheck their current authorization state.

## Preparation And Completion

```cpp
auto policy = jose::header_policy{.typ = "application/example+jws"};
auto prepared = jose::prepare(payload_bytes, selected_public_key, policy);
auto digest = prepared.signing_digest();
// Give this digest to an existing ES256 digest signer without hashing it again.
// The caller owns provider selection, waiting and cancellation.
auto compact = prepared.complete(external_signature_bytes);
```

The public key input is a Forge P-256 uncompressed point: exactly `0x04 || x || y`
in `p256::public_key_point_data`. For an already trusted public JWK, use
`forge.codec.json` to read it, require `kty=EC`, `crv=P-256`, reject private `d`,
and decode `x` and `y` with `forge.codec.base64` URL alphabet and forbidden padding.
Each coordinate must decode to exactly 32 bytes before copying it into the point.
Do not obtain trust keys from the credential's header. The leaf never accepts a
private key, generic JWK JSON or a backend crypto object.

Preparation copies the public key, header and payload into an owning backend
state. `signing_input()` exposes the frozen exact signing input; `signing_digest()`
returns its SHA-256 digest by value. Completion accepts exactly 64 JOSE bytes
`r || s`, not DER, a recoverable signature or Compact text. CJOSE checks the
signature against the frozen key and bytes before exporting Compact. A signature
failure leaves preparation retryable; successful completion consumes the state.
Accessors remain available after success. Export failure after successful backend
completion also consumes the state; reprepare rather than completing it twice.
Move-only prepared states require caller-serialized access, not concurrent
`complete` calls. Independent states and immutable snapshots can be used independently.

## Header And Byte Policy

`alg` is always exactly `ES256`. `header_policy.typ` requires that exact string;
`nullopt` requires `typ` to be absent, never an arbitrary type. With
`key_id_policy::required`, `kid` is required and must match `[A-Za-z0-9._-]{1,128}`.
With `key_id_policy::absent`, the header must omit `kid` and the snapshot must
contain exactly one key whose ID is empty. Unknown fields, embedded keys/URLs,
`crit`, `cty` and `b64` are rejected. There is no algorithm negotiation, detached
payload, JWE, nesting or token-directed I/O.

Maximum byte limits are 16,384 for Compact, 512 for decoded protected headers and
8,192 for decoded payloads. A policy may only reduce them. Compact must have three
nonempty canonical unpadded Base64url segments; the signature is exactly 64 bytes.
Header parsing uses the existing exact JSON codec with bounded depth/nodes/
members/strings and rejects duplicates (including escaped duplicates), invalid
UTF-8, NUL, lone surrogates and trailing documents. Valid whitespace, escapes and
member ordering are accepted. Verification authenticates original Compact bytes,
never parse/reserialize output. Payload remains opaque here; a JWT consumer must
apply its full strict JSON/schema/claim profile separately.

Never log Compact text, payload, signing input or signatures. `prepared_message`
and `verified_payload` opt into Forge diagnostic redaction; explicit byte access
is intentional transmission/validation, not a logging adapter. Boundary errors
use fixed diagnostics and never forward parser or CJOSE/OpenSSL error strings.

## Dependencies And Tests

Public dependencies: `forge_crypto_asymmetric`, `forge_crypto_digest`,
`forge_exceptions`. Private dependencies: `forge_codec_base64`, `forge_codec_hex`,
`forge_codec_json`, `forge_variant`, and the single `cjose::cjose_static` target
shared with existing OAuth consumers. CJOSE, Jansson and OpenSSL types do not
appear in public modules. There are no Auth, Chain, Asio, plugin, network,
scheduler or provider dependencies.

`tests/test_crypto_jose.cpp` uses real CJOSE verification, the RFC 7515 A.3 vector
and existing Forge P-256 signing primitives. It covers owned frozen inputs,
wrong keys/preimages/signature formats, retry/single completion, state movement,
tampering, strict headers, byte boundaries, retained key/snapshot identity and
domain/profile/key rotation isolation. Shared test/package/CI registration is
owned by the parent build configuration, not this leaf. Local build or test
execution is not implied by these source fixtures.
