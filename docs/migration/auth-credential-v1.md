# Migrate To Forge Auth Credential v1

F1 relocates the existing credential model from Pairing to the neutral
`forge_auth_credential` leaf. It is a public C++ source/package break, not a
wire or persisted-storage migration. No compatibility aliases, parallel
credential records, storage-version bump or database reset are introduced.
Apply the Forge release policy for the approved source break; this guide does
not choose a release version or create a release/tag.

The new owner is [Auth Credential](../../libraries/auth/credential/README.md).
[Pairing](../../libraries/auth/pairing/README.md) remains the owner of bootstrap,
pending approval, pre-session tokens and their one-time exchange. Credential
token acquisition, external verification, provisioning, durable storage and
product permissions do not move into the credential leaf.

## Mechanical Source Mapping

Replace the following symbols; keep existing member names and ordering:

| Old symbol in `forge::auth::pairing` | New symbol in `forge::auth::credential` |
| --- | --- |
| `credential_id` | `id` |
| `credential_binding` | `binding` |
| `credential_state` | `state` |
| `credential` | `record` |
| `scope_set` | `scope_set` |
| `canonicalize_scopes` | `canonicalize_scopes` |
| `rotate_credential_downscope` | `rotate_downscope` |
| `revoke_credential` | `revoke` |

Replace credential-only imports with their owning modules:

```cpp
import forge.auth.credential.record;
import forge.auth.credential.serialization; // When using persisted Raw records.
```

The record module exports the credential exception category. An explicit
exception-only consumer may import `forge.auth.credential.exceptions`.
Keep `forge.auth.pairing.pairing`, `types` and `serialization` imports when the
consumer actually handles bootstrap/pending records. Do not blindly replace
every Pairing import: a mixed pairing/credential consumer needs both owners.

`approve_pending` now returns `forge::auth::credential::record`;
`consume_approved_pre_session` returns `forge::auth::credential::binding`.
Their pending-state, approval and replay behavior is unchanged. Pairing callers
pass the neutral `id` in existing approval options.

Session issuance, validation, rotation and device-grant functions now take
`const forge::auth::credential::record&`. Device-grant validation returns the
neutral `binding`; session `principal`, session/device field names and lifetime
mechanics remain unchanged. HTTP `authorize` takes the same neutral record.
Configured credentials and service-resolved external bindings use these
existing functions directly, without creating fake pairing requests.

For example, this is the same pure session transition after the service owner
has checked source trust and loaded the current credential:

```cpp
import forge.auth.credential.record;
import forge.auth.session.session;

forge::auth::session::session_issuance issue_bound_session(
    const forge::auth::credential::record& current,
    forge::auth::session::session_options options) {
   return forge::auth::session::issue_session(current, options);
}
```

Rebuild dependent libraries/programs and their module artifacts together. Do
not retain an old Pairing credential alias or add a mirrored external-identity
record to keep old source compiling. Do not rename persisted consumer fields
such as `credential_id` merely because the owning C++ type is now `id`.

## Targets And Components

A consumer that manipulates credentials directly requests the new component:

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS auth_credential)
target_link_libraries(consumer PRIVATE Forge::forge_auth_credential)
```

Session-only consumers keep `COMPONENTS auth_session` and
`Forge::forge_auth_session`; HTTP consumers keep `auth_http` and
`Forge::forge_auth_http`. Neither needs to request Pairing for credential
declarations. Actual pairing consumers retain `auth_pairing` and
`Forge::forge_auth_pairing`. Source-tree targets use the corresponding names
without `Forge::`.

The credential target directly links `forge_exceptions` and `forge_raw`.
Raw transitively requires Variant, reflection, Chrono, Schema and codec leaves.
Credential itself has no Crypto Digest, Pairing or Session dependency. Session
still needs its own Base64, Crypto Core and Crypto Digest dependencies; removing
Pairing is not permission to remove those leaves from package closure.

## Typed Exceptions

Update catches and category checks according to the operation owner, not by a
global replacement of every pairing error:

- Relocated canonicalization/validation/downscope/revoke failures use
  `forge::auth::credential::exceptions` and category `forge.auth.credential`.
  Their codes remain `invalid_state = 6`, `scope_invalid = 7`,
  `identity_invalid = 8`, `generation_exhausted = 9`,
  `credential_id_invalid = 10`.
- Bootstrap, pending approval and token exchange continue to expose Pairing
  exceptions, including their scope/identity/credential-ID failures.
- Session boundaries retain their typed `invalid_state`, `credential_revoked`,
  `credential_mismatch`, `identity_mismatch` and `scope_mismatch` meanings.
  HTTP continues to propagate these typed session failures.
- Invalid Raw enum/timestamp decoding remains a Raw `codec_error`, not a
  credential lifecycle error. Do not classify failures by message text.

## Unchanged Wire And Stored Records

The relocated layouts retain the old primitive sequence:

- `id`: `value`.
- `binding`: `id`, `generation`.
- `record`: `id`, `identity`, `scopes`, `generation`, `issued_at`, `updated_at`,
  `state`, `revoked_at`.

In stored credential records the state is still one byte: active 0, revoked 1.
The Variant enum names remain `active` and `revoked`. Persisted timestamps remain
signed 64-bit nanoseconds; optional timestamps/bindings retain their presence
encoding. Bootstrap/pending and session/device records retain their field and
enum order. Issuance secrets remain transient; session/device records remain
digest-only.

No issuer claims or source-expiry/freshness fields are newly persisted in the
neutral record or copied into each session row. No new wire protocol or silent
expiry-field extension is part of this relocation. Any actual layout change
requires a separate migration design.

## Source Trust And The Service Owner

The credential owner resolves verified external issuer/subject identity to its
durable binding under its provisioning policy. The verifier alone does not
create the record. Use the owner's stable credential ID and generation; do not
derive them from email, a timestamp or JWT `jti`.

Every admission must check both the current credential binding/generation/state
and source-specific expiry/freshness. A successful session-secret check does not
extend source trust. Use an authoritative local binding snapshot with a finite
valid-until bound; reject new admission when required trust is expired, stale or
unknown. The bound is owner policy, not a new persisted Forge credential field.

External verification, revalidation, refresh and network work happen outside
DB transactions. The owner then uses a current/version-checked binding for the
atomic session/grant transition, without accepting a stale validation snapshot
after a concurrent revoke or generation change. Streams require bounded
rechecks and revocation handling, not perpetual trust after opening.

Without supported revalidation/refresh/revocation, keep sessions short and
bounded by source validity, then require a new login. Membership changes may
require a separate product authorization generation. Atomic refresh, replay
handling, persistence and audit remain service/plugin responsibilities; Forge
pure transitions do not serialize concurrent DB writers.

## Consumer Verification Checklist

1. Apply the source/import/target mapping and update typed catches. Keep actual
   Pairing flows on their existing owner; remove credential-only Pairing usage.
2. Run the Forge `test_forge_auth_credential`, `test_forge_auth_pairing`,
   `test_forge_auth_session` and `test_forge_auth_http` targets and the applicable
   installed-package consumers with the coordinator's single build directory.
3. Use [the legacy writer fixture](../../tests/auth/credential_legacy_writer_tests.cpp)
   to check the old credential, pairing, session and device primitive layouts,
   exact decode/re-encode and enum Variant spellings. Credential records also
   have fixed golden bytes; this is stronger than a new-writer round trip alone.
4. Independently reopen a copy of each consumer's actual old DB without reset.
   Verify its own object/table IDs, indexes, bindings, generations and existing
   session/grant records. If a consumer derives storage metadata from C++ type
   names, explicitly check that the namespace move did not change its mapping.
5. Verify that downscope/generation change and revocation reject old sessions;
   concurrent refresh leaves only one active grant family. Preserve digest-only
   storage and the consumer's atomic rollback/replay semantics.
6. Exercise source expiry, finite binding freshness, revalidation outside DB
   transactions and bounded stream revocation in the consuming service.

The Forge byte fixture and focused tests do not prove downstream full-DB
compatibility, real-issuer SSO or a complete product deployment. Reopen tests for
actual consumer databases belong to those consumers and remain a separate
acceptance gate. This guide makes no upstream/full-DB acceptance claim.
