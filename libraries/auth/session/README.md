# Forge Auth Session

`forge_auth_session` owns product-neutral browser-session and rotating
device-grant records with pure state transitions. It has no database, HTTP,
cookie, plugin, route or product role dependency.

Public modules:

- `forge.auth.session.exceptions`: typed token, expiry, credential-binding and
  state-transition failures.
- `forge.auth.session.types`: digest-only persisted records, issued secrets and
  typed validated principals.
- `forge.auth.session.session`: issuance, validation, CSRF verification, idle
  renewal, rotation, logout and revocation transitions, issuance-integrity
  validation, rotating device-grant transitions, and strict canonical token
  digest identification for caller-owned record lookup.
- `forge.auth.session.serialization`: canonical Raw serialization for persisted
  session records, including strict enum and timestamp validation on decode.

Issuance and rotation create independent 32-byte session and CSRF secrets as
canonical unpadded base64url `forge::crypto::core::secret_string` values. The
persisted `session_record` holds only SHA-256 digests plus a neutral credential
ID, generation, identity and canonical scopes. No API accepts caller-selected
new secret material. `validate_issuance` accepts only active issuances,
structurally validates their records and constant-time verifies both canonical
clear secrets against persisted digests before a transport emits them.

Callers provide trusted, non-decreasing wall-clock `now` values. Records retain
these timestamps across restart and reject transitions earlier than persisted
creation or activity times. Absolute and idle expiry are explicit; idle renewal
never extends beyond absolute expiry.

A device grant stores only its token digest and neutral credential binding. It
can issue a replacement short-lived session only after the caller validates the
current credential. Rotation preserves the original absolute expiry and leaves
the previous digest in a terminal `rotated` record so a caller-owned store can
detect replay and atomically revoke the grant family and its sessions.
`refresh_device_grant` constructs the replacement grant and short-lived session
before terminally rotating the prior record. Its absolute expiry cannot exceed
the grant expiry. The caller must load and consume the active grant under a row
lock or compare-and-swap on `(token_digest, state, rotation_generation)`, then
persist the rotated predecessor, replacement grant and session in that same
transaction. Exactly one active grant is permitted for a credential generation;
this lets a proven replay revoke that credential's active grant and sessions.

Issuance/validation consume `forge::auth::credential::record`, whether the owner
provisioned a configured credential, bound a verified external identity or
approved pairing. Sessions do not import or link pairing. This relocation
preserves all session/device fields and Raw layouts; see
[Auth Credential](../credential/README.md) for mechanical source migration and
the legacy writer fixture. Source expiry/freshness is checked by the service
owner on every admission; successful session validation does not replace that
check or extend external-token trust. External issuer claims are not copied
into each session row. Consumer DB reopen/index tests remain consumer-owned.

Dependencies: `forge_auth_credential`, `forge_codec_base64`, `forge_crypto_core`,
`forge_crypto_digest`, `forge_exceptions`, `forge_raw`.
