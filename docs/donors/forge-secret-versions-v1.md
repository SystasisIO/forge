# Immutable Secret Versions: Donor Traceability

Date: 2026-10-15. Inspected Forge base:
`8975f234dabf2cbfb91d766bf32194f3d8832a5b`.
Status: source evidence for the [proposed design](../iterations/forge-secret-versions-v1.md),
not runtime acceptance.

| Inspected owner | Accepted pattern | Target / required proof |
| --- | --- | --- |
| [secrets API](../../plugins/crypto/secrets/include/forge/plugins/crypto/secrets/api.cppm), [types](../../plugins/crypto/secrets/include/forge/plugins/crypto/secrets/types.cppm) | Local typed capabilities, existing operation DTOs; old contract stays intact | Separate local write/metadata/version slices; old descriptor/DTO golden and consumer tests |
| [API macros](../../libraries/api/core/include/forge/api/core/macros.hpp), [descriptor](../../libraries/api/core/include/forge/api/core/descriptor.cppm), [local handle](../../libraries/api/core/include/forge/api/core/handle.cppm) | Local-only contracts disable raw codec generation and invoke native methods | Native move-only secret_bytes create input; compile/register/call and abandoned/canceled ownership tests; no new secure buffer or serializer |
| [secrets config](../../plugins/crypto/secrets/config.cpp) | Typed Schema/Config, encrypted source loading, byte/KDF ceilings | Additive managed config descriptor; old value/file/encrypted_file coverage and secret redaction |
| [secrets implementation](../../plugins/crypto/secrets/plugin_impl.cpp), [loaded resource](../../plugins/crypto/secrets/details/loaded_secret.hxx) | Immutable atomic resource publication, purpose/operation allow-lists and raw-export gate | Shared operation implementation; retained exact version; revoke admission race tests |
| [encrypted container](../../libraries/crypto/keystore/encrypted_file.cpp), [container types](../../libraries/crypto/keystore/include/forge/crypto/keystore/types.cppm) | Existing FORGEKS1 authenticated header, scrypt ceilings, AES-GCM, secure plaintext buffers | Reuse encrypt_file/decrypt_file without crypto/wire changes; truncation/corruption/wrong-bootstrap tests |
| [ownership](../../libraries/crypto/keystore/ownership_impl.cpp), [public ownership](../../libraries/crypto/keystore/include/forge/crypto/keystore/ownership.cppm) | Persistent sibling lock, process/inode checks, exclusive claim, private regular files | No second lock; process/fork/replaced-lock/mode tests |
| [keystore store persistence](../../libraries/crypto/keystore/store_impl.cpp) | Bounded owner-only read, private directory chain, temp+file fsync+atomic publication+directory fsync, distinct post-publication durability_unknown | Extract shared persistence within keystore leaf; preserve old signing payload/API and existing tests; injected I/O failures |
| [wallet plugin lifecycle](../../plugins/crypto/wallet/plugin.cpp), [admitted wallet work](../../plugins/crypto/wallet/details/provider_impl.hxx) | Application compute injection, bounded gate, isolated admitted compute wait, fault fence and retained ownership | Managed init/crypto/mutation use context.compute; stop/cancel/rename and shutdown-drain tests |
| [secure bytes](../../libraries/crypto/core/include/forge/crypto/core/secret_bytes.cppm), [secure strings](../../libraries/crypto/core/include/forge/crypto/core/secret_string.cppm) | Move-only material, diagnostic-secret hooks and explicit wiping | No plaintext on disk or in metadata/error/log output; bounded/wiped transient encoding |
| [bootstrap password reader](../../libraries/crypto/keystore/password.cpp) | Owner-only regular file, no-follow, bounded single line, preserved non-newline whitespace, wiped errors | Separate external passphrase file; no plugin-local reader or managed-key recursion |
| [HMAC](../../libraries/crypto/digest/include/forge/crypto/digest/hmac.cppm) | Existing keyed SHA-256 digest mechanism | Per-store encrypted receipt key and versioned canonical comparison; no raw or unkeyed material hash in metadata |
| [Raw limits](../../libraries/raw/include/forge/raw/codec.cppm), [stream allocation budget](../../libraries/raw/include/forge/raw/stream.cppm) | Exact decode with per-container, cumulative element and per-byte-allocation limits | Explicit storage-specific budget; hostile counts/lengths fail before large allocation; no new codec |
| [current tests](../../tests/plugins/crypto/secrets_tests.cpp), [installed consumer](../../tests/package_plugins_crypto_secrets/main.cpp) | Configured-source, purpose/operation, byte/KDF and local-API compatibility baseline | Retain baseline; add managed restart/replay/revoke/fault tests and installed imports |

Rejected patterns:

- Reusing signing `store::put` for arbitrary bytes or changing its serialized
  entry vector: signing keys and managed secret records have different domains.
- Copying lock/rename/fsync into the plugin: shared filesystem mechanics already
  exist in the keystore leaf and need one implementation.
- Copying legacy `apply_config`'s final `try_admit` after a durable mutation:
  stop could otherwise leave disk changed and memory old.
- Promoting a leftover temporary or falling back to an old backup when the
  stable file is corrupt: either can revive revoked authority.
- Plaintext/idempotency journals, externally visible material hashes, automatic
  token eviction, unbounded history, and reset-on-missing-bootstrap.
- A new crypto primitive, encrypted file format, parser, thread pool, detached
  worker or network request path for local reads.

No external donor is a new build dependency. All accepted mechanisms already
belong to Forge. This note does not assert that a source audit proves durability,
compatibility or runtime acceptance; those require the design's concrete tests.
