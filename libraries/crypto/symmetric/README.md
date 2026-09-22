# Forge Crypto Symmetric

`forge_crypto_symmetric` owns authenticated and conventional symmetric
encryption plus key derivation. Package component: `crypto_symmetric`. Public
namespace: `forge::crypto::symmetric`.

## Modules

- `forge.crypto.symmetric.aes`
- `forge.crypto.symmetric.chacha20_poly1305`
- `forge.crypto.symmetric.kdf`
- `forge.crypto.symmetric.xsalsa20`
- `forge.crypto.symmetric.salsa20`

```cpp
import forge.crypto.symmetric.aes;

const auto key = forge::crypto::symmetric::aes::generate_aes256_key();
```

`forge.crypto.symmetric.xsalsa20` is a stream-cipher boundary backed by a
private, unmodified libsodium 1.0.22 subset. `xsalsa20::key` owns its 32-byte
secret through `forge.crypto.core.secret_bytes`; `xsalsa20::nonce` is exactly 24
bytes. `xsalsa20::stream` transforms supplied mutable spans in place and retains
partial 64-byte blocks across calls, so callers can use separate instances for
independent read and write directions. Arbitrary counter positioning is not
exposed: a stream starts at counter zero and owns its complete counter range.
Every independent stream or direction under a key requires a unique nonce.
Never reuse the same key and nonce pair; separate read and write streams under
one key must use distinct nonces.

XSalsa20 provides no authentication. A caller must pair it with a suitable
authentication boundary when active modification is in scope. The public module
does not expose libsodium headers, types or functions.

The target depends on `forge_crypto_core`, `forge_exceptions`, OpenSSL Crypto
and the private vendored XSalsa20 object target. It does not load secrets, own
configuration or provide a vault; those are application/plugin responsibilities.
`test_forge_crypto_symmetric` covers roundtrips, authentication failures,
streaming, XSalsa20 vectors/counter behavior and KDF limits.

## Salsa20 Keystream

`salsa20::key` is a move-only 32-byte secret; `salsa20::nonce` contains exactly
8 bytes. `make_nonce(bytes)` validates an existing nonce, not generates one.
`keystream(key, nonce, output_size)` returns the requested number of bytes from
counter zero. Zero output is accepted after validating the key; oversized
container requests throw `salsa20::exceptions::invalid_size`. Wrong key/nonce
lengths and moved-from keys produce typed errors. Allocation failure remains
`std::bad_alloc`; callers must bound externally supplied output sizes.

```cpp
#include <span>
import forge.crypto.core.secret_bytes;
import forge.crypto.symmetric.salsa20;

// key_bytes and nonce_bytes are supplied by the caller's protocol.
const auto key = forge::crypto::symmetric::salsa20::key{key_bytes};
const auto nonce = forge::crypto::symmetric::salsa20::make_nonce(nonce_bytes);
const auto secret = forge::crypto::core::secret_bytes{
    forge::crypto::symmetric::salsa20::keystream(key, nonce, 64)};
```

The returned keystream is secret: erase it or immediately own it in
`secret_bytes`, including on exception paths. Repeated calls restart at zero;
they do not continue a stream. Never reuse a key/nonce pair for independent
encryption. Salsa20 is unauthenticated; prefer authenticated encryption for new
message protocols. This API is not XSalsa20 and does not accept its 24-byte nonce.
The implementation reuses the already compiled, unmodified libsodium Salsa20
reference path through a private shim, without another target or component.
Tests include the pinned libsodium `stream2` golden, block-boundary prefixes,
invalid sizes, moved keys and installed-consumer symbol coexistence.
