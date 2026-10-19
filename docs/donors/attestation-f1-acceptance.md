# JWT Attestation F1 Acceptance

Status: implementation candidate; CI and final independent review pending.

## Scope

This change supplies shared attestation values, a native typed signing API and
the qualified CJOSE adapter. It also extends existing schema/JSON machinery for
exact dynamic objects and exportable constraints. It does not implement the
signing plugin, custody orchestration, issuer journal or product authorization.

The approved plan is recorded in
[Foundation PR 19](https://github.com/SystasisIO/foundation/pull/19).
Source base: Forge `7061cd35` plus qualified CJOSE branch
`2f35de5e860ed4cf0cb4179c25bcf54a9c7e4b40`. The CJOSE submodule remains
`84da8b54feb4e46ea1ddae6930f821e1510471b7`.

## Ownership And Reuse

- `auth.attestation` owns one shared `document<Claims>` declaration and pure
  profile validation. Native schema metadata is excluded from the guest module
  and dependency sets.
- `auth.api.attestation_signer` owns the local/remote service contract and uses
  the existing authenticated caller and server-supplied argument machinery.
- `crypto.jose` owns public-key-only preparation/completion and verification
  through CJOSE. It retains actual key and immutable trust snapshot identity.
- `schema` owns constraints and the common exporter; `variant.schema` owns
  the dynamic-object adapter. The existing schema input representation is
  extracted, not duplicated. JSON remains backed by Glaze.
- The independent artifact consumer uses the existing offline Chain ABI
  converter. Only this test consumer depends on Chain; the neutral auth and
  JOSE APIs do not.
- Existing Contract SDK generation, guest runtime and compiled-module invocation
  execute the WASM fixture. No second dispatcher, VM or coroutine runtime is
  introduced.

## Required Evidence

| Gate | Owner / evidence | Current result |
|---|---|---|
| J1/J5/J8 | `test_forge_crypto_jose`: RFC vector, exact bytes, signed malformed headers, wrong key/type, immutable preparation and snapshot separation | NOT_RUN |
| J2/J3 | `tests/attestation_interop`: pinned joserfc and Nimbus, both directions, independently signed negative corpus | NOT_RUN |
| J4 | `guest/tests/attestation`, build companion and independent `forge_attestation_boundary`; bound ABI/metadata/schema artifacts, actual WASM Raw parity, wide counters | NOT_RUN |
| J6 | `test_forge_json_strict` and schema tests: duplicate/escaped keys, Unicode/NUL/trailing input, exact kinds and limits; ASan/UBSan and bounded signed-byte mutation corpus | NOT_RUN |
| J7 | `test_forge_auth_attestation`: time, lifetime/skew, checked bounds and document ID format; existing crypto RNG in native test issuer | NOT_RUN |
| J9/Q8b | Installed `tests/package_attestation`, actual guest SDK, optional backend isolation and structural gates | NOT_RUN |
| J10 | `test_forge_attestation_policy`: exact object/range/provider, actual key/delegation, revoked generation/stale state, expiry/request rechecks on cache hits, independent snapshot identities and key/header-policy changes | NOT_RUN |
| Regressions | Existing Raw, variant, JSON, schema, API, OpenAPI and liboauth2 tests | NOT_RUN |

The workflow `attestation.yml` runs Linux and macOS against the exact pushed
commit. Build diagnostics and whitelisted results are retained; token/private
material and generated build trees are not uploaded. A passing source check or
plan review is not runtime acceptance. F0-B evidence is historical backend
qualification and is not relabelled as F1 acceptance.

The first implementation review returned NEEDS_CHANGES. Its regression fixes
cover nested writer validation, exact-decode budget preservation, non-widening
shape refinement and Unicode length parity. Product schema lengths count Unicode
scalar values; parser/profile byte limits remain separate. The J10 policy/cache
is an explicit test fixture, not a production issuer, second ACL or distributed
revocation implementation.

## Downstream Gates

F2 adds the single signing-plugin implementation and policy/lifecycle tests.
P1 publishes the service in the existing signer process. G2 adds durable effects,
response-intent checking and recovery. S0/S1 define product claims, delegation,
holder binding and provider revocation. These gates are not implemented by F1.
