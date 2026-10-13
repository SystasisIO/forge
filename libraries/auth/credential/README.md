# Forge Auth Credential

`forge_auth_credential` is the sole product-neutral owner of credential IDs,
bindings, states, records, canonical scopes and validation. Pairing is one
issuance mechanism, not a prerequisite for sessions. This leaf owns no token
issuance, OAuth verification, issuer/subject mapping, database or product policy.

Public modules:

- `forge.auth.credential.record`: `id`, `binding`, `state`, `record`,
  `scope_set`, field/record validation, canonicalization, downscope and revoke.
- `forge.auth.credential.exceptions`: typed ID, identity, scope, state and
  generation failures in the `forge.auth.credential` category.
- `forge.auth.credential.serialization`: the unchanged Raw record layout,
  including strict enum and timestamp checks on decode.

The owner supplies a stable non-empty ID, identity, sorted unique non-empty
scopes, a positive generation and trusted non-decreasing wall-clock values.
`rotate_downscope` cannot escalate or retain identical scopes; it increments
generation only after validation succeeds. `revoke` is terminal. Failed
transitions leave the input unchanged. Pairing keeps its original boundary
exception categories; session validation still reports session-specific
`invalid_state`, `credential_revoked` and binding/identity/scope mismatches.
Calls to the relocated transitions now catch credential exceptions, not pairing
exceptions. The error values for these moved failures remain 6 through 10.

## Source Migration

The full consumer checklist is in
[Auth Credential v1 migration](../../../docs/migration/auth-credential-v1.md).

This is an approved public source break, not a storage-format migration. No
compatibility aliases or second record model exist. Include/import and link the
new owner directly when manipulating credentials:

```cpp
import forge.auth.credential.record;
import forge.auth.credential.serialization; // Only for persisted Raw records.
```

| Previous pairing symbol | Credential symbol |
|---|---|
| `forge::auth::pairing::credential_id` | `forge::auth::credential::id` |
| `forge::auth::pairing::credential_binding` | `forge::auth::credential::binding` |
| `forge::auth::pairing::credential_state` | `forge::auth::credential::state` |
| `forge::auth::pairing::credential` | `forge::auth::credential::record` |
| `forge::auth::pairing::scope_set` | `forge::auth::credential::scope_set` |
| `forge::auth::pairing::canonicalize_scopes` | `forge::auth::credential::canonicalize_scopes` |
| `forge::auth::pairing::rotate_credential_downscope` | `forge::auth::credential::rotate_downscope` |
| `forge::auth::pairing::revoke_credential` | `forge::auth::credential::revoke` |

Request `find_package(Forge CONFIG REQUIRED COMPONENTS auth_credential)` and
link `Forge::forge_auth_credential`. Session-only consumers keep requesting
`auth_session` and linking `Forge::forge_auth_session`; they no longer need
pairing. Actual bootstrap/pending approval continues to use `auth_pairing`.

## Persistence And Trust

The record retains the exact order `id`, `identity`, `scopes`, `generation`,
`issued_at`, `updated_at`, `state`, `revoked_at`. The state bytes remain active
0 and revoked 1; Variant spellings remain `active` and `revoked`. Timestamps
remain signed nanoseconds and optional fields retain their presence bytes.
No issuer claims, expiry fields or new schema version are added.

[Legacy writer fixtures](../../../tests/auth/credential_legacy_writer_tests.cpp)
independently write the old primitive sequence for credentials, pairing,
sessions and device grants, then check exact decode and re-encode. Credential
records also have fixed golden bytes. The fixture is not a consumer database
reopen test: consumers must verify their own DB IDs, indexes and persisted
records without resetting the database.

The service owner resolves external identities to this record and checks
source-specific expiry/freshness on each admission, outside any remote work
inside a DB transaction. A session secret alone cannot extend source trust.
An external identity binding is not proof of token verification. Generation,
membership changes, atomic refresh/revocation and stream rechecks are owner
responsibilities. Never log clear bearer/session secrets or store plaintext
session tokens; the existing session records remain digest-only.

Direct dependencies: `forge_exceptions`, `forge_raw`. Raw transitively requires
Variant, Chrono, Schema, reflection and codec components; credential does not
depend on Crypto Digest, Pairing, Session or OAuth adapters.
