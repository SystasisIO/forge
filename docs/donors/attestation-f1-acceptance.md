# JWT Attestation F1 Acceptance

Delivery scope: native JWT/API, schema/JSON and package integration only.
WASM qualification is DEFERRED until the WAMR delivery from R&D. Per-candidate
CI and independent review results are recorded in the integration pull request;
the requirements below do not substitute for those immutable results.

## Scope And Qualified Predecessor

This change supplies shared attestation declarations, a native typed signing
API and the qualified CJOSE adapter. It extends the existing schema/JSON
machinery for exact dynamic objects and exportable constraints. It does not
implement a signing plugin, custody orchestration, issuer journal, product
authorization, guest SDK registration or WASM execution.

The approved plan is recorded in
[Foundation PR 19](https://github.com/SystasisIO/foundation/pull/19).
The predecessor `71fc8a2890c3ea5561afa536b1879da8a3334270` received two independent
NATIVE-ONLY CLEAN reviews. Its actual
[CI evidence](https://github.com/SystasisIO/forge/actions/runs/39305173493)
contains 23/23 native tests, 393/393 interoperability checks, 120/120 schema/model
cases and successful installed-package controls on Linux and macOS, plus 5/5
sanitizer tests. That workflow remains FAILURE because its old Linux guest
phase failed; it is not full F1/WASM acceptance.

The native integration preserves those library implementations and incorporates
the current `dev` baseline without carrying the predecessor's guest fixtures,
guest build changes or sysroot repair. Original guest work remains on the
predecessor branch for the R&D handoff. The CJOSE submodule remains
`84da8b54feb4e46ea1ddae6930f821e1510471b7`.

## Ownership And Reuse

- `auth.attestation` owns one shared `document<Claims>` declaration and pure
  profile validation. Its native schema metadata remains separate from the
  shared value declarations; no qualified guest component is shipped here.
- `auth.api.attestation_signer` owns the local/remote service contract and uses
  the existing authenticated caller and server-supplied argument machinery.
- `crypto.jose` owns public-key-only preparation/completion and verification
  through CJOSE. It retains actual key and immutable trust snapshot identity.
- `schema` owns constraints and the common exporter; `variant.schema` owns
  the dynamic-object adapter. The existing schema input representation is
  extracted, not duplicated. JSON remains backed by Glaze.
- The existing `forge_add_contract_library` helper declares the native target;
  its location under `guest/cmake` is not a new guest runtime dependency. No
  dispatcher, VM, coroutine runtime or native product claims registry is added.

## Required Native Evidence

| Gate | Evidence | Boundary |
|---|---|---|
| J1/J5/J8 | `test_forge_crypto_jose`: RFC vector, exact bytes, signed malformed headers, wrong key/type, immutable preparation and snapshot separation | Native |
| J2/J3 | `tests/attestation_interop`: pinned joserfc and Nimbus, both directions, independently signed negative corpus | Native |
| J6 | `test_forge_json_strict` and schema tests: duplicate/escaped keys, Unicode/NUL/trailing input, exact kinds and limits; ASan/UBSan and bounded signed-byte mutation corpus | Native |
| J7 | `test_forge_auth_attestation`: time, lifetime/skew, checked bounds and document ID format; existing crypto RNG in native test issuer | Native |
| J9/Q8b | Installed `tests/package_attestation`, qualified adjacent/external backend discovery, isolation/negative controls and structural gates | Native only; guest SDK deferred |
| J10 | `test_forge_attestation_policy`: exact object/range/provider, actual key/delegation, revoked generation/stale state, expiry/request rechecks on cache hits, independent snapshot identities and key/header-policy changes | Native fixture, not production authorization |
| Regressions | Existing Raw, variant, JSON, schema, API, OpenAPI and liboauth2 tests | Relevant native dependencies |
| J4 | Actual guest ABI/Raw execution, artifact binding and independent installed SDK consumer | DEFERRED to WAMR R&D |

The workflow `attestation.yml` qualifies only the native delivery on Linux and
macOS against the exact pushed commit. Build diagnostics and whitelisted results
are retained; tokens, private material and generated build trees are not
uploaded. Source review alone is not runtime acceptance. F0-B results remain
historical backend qualification and do not replace F1 native evidence.

Native validation includes nested writer checks, exact-decode budgets,
non-widening shape refinement and Unicode length parity. Product schema lengths
count Unicode scalar values; parser/profile byte limits remain separate.
Generated Pydantic models alone are not the full validator: the qualified
schema-first consumer rejects three known decimal coercions. The J10 policy/cache
is an explicit test fixture, not a production issuer, second ACL or distributed
revocation implementation. Synthetic measurements do not establish production
performance or count backend/executor allocations.

## Downstream Gates

F2 adds the single signing-plugin implementation and policy/lifecycle tests.
P1 publishes the service in the existing signer process. G2 adds durable effects,
response-intent checking and recovery. S0/S1 define product claims, delegation,
holder binding and provider revocation. These gates, and the deferred WAMR
qualification, are not implemented or accepted by this native delivery.
