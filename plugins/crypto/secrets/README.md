# Secret Provider Plugin

## Lifecycle Migration

Public lifecycle entry points are final framework wrappers; implementations use
protected `on_*` hooks with unchanged arguments. See the
[shared migration contract](../../README.md#lifecycle-migration). No compatibility aliases
or plugin-local schedulers are provided.

Configured material is an immutable, atomically published snapshot. Admitted
crypto operations retain that snapshot and may finish after stop; new calls
fail with the typed `exceptions::stopped`. Read-only status remains available.
This does not promise immediate destruction of material retained by an API owner.

`forge::plugins::crypto::secrets` provides a local-only secret operations API for
`forge_app` applications. It lets application plugins request bounded secret
retrieval, HKDF-SHA256 derivation and AES-GCM encryption/decryption by
`secret_id` and `purpose`, without parsing secret-bearing config or encrypted
files in product code.

The plugin is not a wallet, remote KMS, product vault or authorization service.
It enforces configured purposes, operation allow-lists, byte limits and local
source loading policy, then delegates cryptographic primitives to
`forge_crypto_core` and `forge_crypto_symmetric`.

## Target And Modules

- Target: `forge_plugins_crypto_secrets`
- Package component: `plugins_crypto_secrets`
- Plugin id: `forge.plugins.crypto.secrets`
- Main API contract id: `forge.plugins.crypto.secrets`
- Config section: `plugins.crypto.secrets`

Public modules:

- `forge.plugins.crypto.secrets.plugin` owns `plugin::descriptor()` and the canonical factory.
- `forge.plugins.crypto.secrets.api`
- `forge.plugins.crypto.secrets.types`
- `forge.plugins.crypto.secrets.exceptions`
- `forge.plugins.crypto.secrets.version_types`
- `forge.plugins.crypto.secrets.version_write_api`
- `forge.plugins.crypto.secrets.version_metadata_api`
- `forge.plugins.crypto.secrets.version_api`

## When To Use

- Product plugins need local secret retrieval, derivation or AES-GCM operations
  through a typed API.
- Secret sources should be decoded and redacted through Forge config/schema.
- Callers should address secrets by `secret_id` and operation `purpose`, not by
  raw key material.

## When Not To Use

- Do not use this plugin as a remote vault, hardware KMS or product
  authorization system.
- Do not store raw secrets in generated examples, logs or diagnostics.
- Do not bypass purpose/operation allow-lists by exporting raw secrets unless a
  local config explicitly permits it.

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_codec_base64`
- `forge_codec_hex`
- `forge_crypto_core`
- `forge_crypto_symmetric`
- `forge_crypto_keystore`
- `forge_crypto_digest`
- `forge_raw`
- `forge_asio`
- `forge_config_core`
- `forge_schema`

## Configuration

Config is decoded through `BOOST_DESCRIBE_STRUCT`, `forge_schema` rules and
`forge_config_core`. Secret-bearing fields are schema-marked as secret so generated
diagnostics and redaction paths do not expose raw material.

```yaml
plugins:
  crypto:
    secrets:
      default-max-plaintext-bytes: 1048576
      default-max-ciphertext-bytes: 1048708
      default-max-aad-bytes: 1048576
      secrets:
        - id: service/session-key
          kind: symmetric_key
          source:
            type: value
            encoding: hex
            value: "<redacted secret bytes>"
          purposes: ["api.payload.decrypt"]
          operations: ["decrypt_aes_gcm", "derive_hkdf_sha256"]
          allow-raw-export: false
```

Supported source types are `value`, `file` and `encrypted_file`. The
`encrypted_file` source uses the plugin's encrypted secret file container helper
and configurable scrypt ceilings.

## Managed Versions

The optional `versions` subsection enables a separate immutable version store.
Existing configured ids retain their old lookup behavior. The versioned API
requires a positive explicit version and never resolves a latest/default version
or falls back to configured material.

```yaml
plugins:
  crypto:
    secrets:
      versions:
        enabled: true
        mode: open_existing
        path: /var/lib/service/secrets/versions.fks
        bootstrap-passphrase-file: /var/lib/service/bootstrap-passphrase
        ceiling:
          purposes: ["session.derive"]
          operations: ["derive_hkdf_sha256"]
          allow-raw-export: false
          max-plaintext-bytes: 4096
          max-ciphertext-bytes: 8192
          max-aad-bytes: 4096
```

Use `mode: create_new` only for explicit first provisioning; it refuses to
overwrite an existing store. Normal startup uses `open_existing`, which fails
when the stable file is missing. The directory must be owner-only (`0700`) and
the bootstrap must be a private regular file outside that directory. Preserve
the persistent sibling `.lock` file while any owner is running.

Enable the application's bounded compute executor before startup, for example
`builder.compute({.worker_threads = 2})`. Managed initialization, recovery,
serialization, file operations and crypto use that executor. Configure/provide
alone does not make either API ready; run the normal application startup.
The older configured-source path retains its existing configure-time loading
and inline crypto scheduling.

Three local contracts separate operations: `version_write_api` creates/revokes,
`version_metadata_api` describes/lists safe metadata, and `version_api` performs
explicit-version secret operations. Their ids are respectively
`forge.plugins.crypto.secrets.version_write`, `.version_metadata` and `.version`,
all version 1.0. A write or metadata handle does not confer a material-read
operation. Local registry discovery is trusted native composition; applications
own authentication and decide which handles their callers can obtain.

```cpp
import forge.plugins.crypto.secrets.version_write_api;
import forge.plugins.crypto.secrets.version_api;
import forge.plugins.crypto.secrets.version_types;

auto writer = context.apis().get<forge::plugins::crypto::secrets::version_write_api>(
   forge::plugins::crypto::secrets::version_write_api::ref());
auto reader = context.apis().get<forge::plugins::crypto::secrets::version_api>(
   forge::plugins::crypto::secrets::version_api::ref());

// material is an already owned forge::crypto::core::secret_bytes.
auto created = co_await writer->create_version({
   .mutation_id = mutation_id,
   .secret_id = "session/root",
   .expected_revision = 0,
   .policy = {
      .purposes = {"session.derive"},
      .operations = {forge::plugins::crypto::secrets::operation::derive_hkdf_sha256},
   },
   .material = std::move(material),
});
auto derived = co_await reader->derive_hkdf_sha256({
   .version = created.ref.version,
   .request = {
      .secret_id = created.ref.secret_id,
      .purpose = "session.derive",
      .salt = salt,
      .info = info,
      .output_size = 32,
   },
});
co_await writer->revoke_version({
   .mutation_id = revoke_id,
   .ref = created.ref,
   .expected_revision = created.revision,
});
```

Each secret has a revision incremented by successful create/revoke operations.
`expected_revision` provides compare-and-swap; use the current metadata revision
when another mutation may have intervened. A mutation id is store-wide and at
most 128 bytes. An exact retry returns its original reference/revision before
checking stale CAS; changing its material, policy, method or CAS conflicts.
Retrying a create after revocation returns the original reference without
reactivating it. A zero policy byte limit resolves to the configured ceiling
only on the first accepted create; retries retain the original resolution.

Policies must remain within the configured purpose, operation, export and byte
ceilings. Startup rejects a narrower ceiling that excludes a retained available
version. Revoked versions remain revoked and do not prevent narrowing.
Metadata exposes references, current revisions, kind, state and policy, without
material, material length, mutation ids or receipt fingerprints. Listing is
paged, with a default page size of 32 and a maximum of 128.

Defaults bound the store to 256 ids, 1024 versions, 32 versions per id, 64 KiB per
material, 4 MiB total material and 8 MiB serialized plaintext. Pending work is
limited to 16 calls and 1 MiB of input; configuration/schema set finite hard
ceilings. There is no deletion or receipt eviction. A full store rejects new
creates while reserved capacity still permits revoking every available version.

## Managed Durability And Recovery

The store reuses the keystore encrypted container and persistent ownership.
A successful mutation acknowledges a file-synced, atomically published and
directory-synced encrypted snapshot containing material, revocations and replay
receipts. No plaintext journal or material fingerprint is exposed.

Pre-publication write failures leave the previous snapshot authoritative and
allow retry. A post-publication directory-sync failure returns
`exceptions::durability_unknown` and fences new operations. Stop the owner,
reopen the stable file, and retry the exact mutation id/request to reconcile
the result. Do not replace the stable file with a backup or temporary file.
Stable corruption, wrong/missing bootstrap or ownership loss fail closed;
encrypted leftover temporaries are never promoted. Startup bounds directory
enumeration and cleanup; an excessive directory needs operator cleanup.

Stop closes new admission, then shutdown drains accepted work and releases
ownership. Accepted writes reconcile even if their caller is canceled after
admission. Revocation denies newly selected reads; an already retained resource
may finish. It does not erase bytes already returned or copies held by callers.

## Examples

Register the descriptor with the application shell and acquire the typed API
from the plugin context:

```cpp
registry.register_plugin(forge::plugins::crypto::secrets::plugin::descriptor());
```

```cpp
auto secrets = context.apis().get<forge::plugins::crypto::secrets::api>(
   {.id = {"forge.plugins.crypto.secrets"}, .major = 1});

auto derived = co_await secrets->derive_hkdf_sha256(
   forge::plugins::crypto::secrets::derive_request{
      .secret_id = "service/session-key",
      .purpose = "api.payload.decrypt",
      .salt = salt,
      .info = info,
      .output_size = 32,
   });
```

## Security And Boundaries

- The plugin owns local secret loading, byte limits and operation gating.
- Products own policy: who may call the API, what a purpose means and where
  secret material originates operationally.
- Do not expose raw secrets unless the config explicitly sets
  `allow-raw-export: true` and the requested operation is allowed.
- Do not use this plugin as a remote vault or product authorization layer.

## Common Mistakes

- Treating `purpose` as decorative metadata. It is part of the operation gate.
- Logging secret ids together with sensitive operational context.
- Using `get_bytes` for data-plane encryption when `encrypt_aes_gcm` or
  `derive_hkdf_sha256` would keep raw material inside the plugin boundary.

## Tests

- `test_forge_plugins`
- `test_forge_package_plugins_crypto_secrets`
- `test_forge_crypto_secrets_versions`
