# Secret Provider Plugin

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
- Using `get_secret` for data-plane encryption when `encrypt_aes_gcm` or
  `derive_hkdf_sha256` would keep raw material inside the plugin boundary.

## Tests

- `test_forge_plugins`
- `test_forge_package_plugins_crypto_secrets`
