# Forge Auth Pairing

`forge_auth_pairing` owns product-neutral bootstrap and device-pairing records
with deterministic state transitions. It has no database, HTTP, UI, route,
product-role or approval-policy dependency.

Public modules:

- `forge.auth.pairing.exceptions`: typed token, replay, expiry, capacity,
  state, scope and identity failures.
- `forge.auth.pairing.types`: digest-only persisted records and explicit
  transition options.
- `forge.auth.pairing.pairing`: bootstrap, pending approval and exchange transitions.
- `forge.auth.pairing.serialization`: canonical Raw serialization for persisted
  pairing records, including strict enum and timestamp validation on decode.

`begin_bootstrap` generates 32 random bytes and returns the clear bootstrap
secret as `forge::crypto::core::secret_string`, encoded with canonical unpadded
base64url. `bootstrap_record` stores only its SHA-256 digest. `consume_bootstrap`
accepts the secret, validates it with a constant-time digest comparison, marks
the record consumed, and returns `pending_issuance`: a pending record plus a
fresh canonical pre-session secret. A pending record stores only
`pre_session_digest` and `pre_session_consumed`; it never stores the clear
pre-session value. The pre-session digest differs from the bootstrap digest,
and supersession returns a fresh distinct pre-session issuance before marking
the prior request superseded.

`identify_bootstrap_token` performs the same strict canonical base64url
validation and returns the SHA-256 digest for indexed bootstrap-record lookup.
It does not consume or otherwise mutate the located record.
`identify_pre_session` performs strict canonical base64url validation and
returns the SHA-256 digest for a caller's pending-record lookup.
`validate_pre_session` verifies the located record without duplicating codec
logic. It accepts pending, approved, rejected and superseded records for
authenticated status polling until expiry, unless the pre-session secret was
consumed. Approval persists an exact `forge::auth::credential::binding` containing the new
credential ID and generation, and does not consume the pre-session secret.
`consume_approved_pre_session` returns that binding as its one-time exchange
result: only an approved, unexpired, unconsumed record succeeds; retries are
replay failures and rejected or superseded records cannot exchange.

Callers supply `now`, expiry and pending capacity explicitly. `now` is a
trusted, non-decreasing wall-clock value retained in persisted records across
restart; transitions reject timestamps earlier than persisted creation or
update times. Scope sets are sorted, unique and non-empty. Approval receives a non-empty stable
`forge::auth::credential::id` and creates a neutral
`forge::auth::credential::record` with that distinct ID, identity, scopes and
generation 1. Common validation, scope canonicalization, later downscope and
revocation belong to [Auth Credential](../credential/README.md), not pairing.
Bootstrap/pending tokens and approval semantics remain here; their error
categories and persisted layouts are unchanged. There are no legacy credential
aliases in the pairing namespace. See the credential README for source migration.

Dependencies: `forge_auth_credential`, `forge_codec_base64`, `forge_crypto_core`,
`forge_crypto_digest`, `forge_exceptions`, `forge_raw`.
