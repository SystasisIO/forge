# Forge Crypto Core

`forge_crypto_core` owns the low-level memory primitives shared by Crypto
libraries. Package component: `crypto_core`. Public namespace:
`forge::crypto::core`.

The Contract SDK exposes only the existing `forge.crypto.core.types` byte-value
module through guest component `forge.crypto.core`. Secret memory, randomness,
constant-time host routines and OpenSSL are not guest APIs. This partial guest
surface does not reuse native archives or introduce a second bytes type.

## Modules

- `forge.crypto.core.types`: owned byte containers.
- `forge.crypto.core.secret_bytes`: move-only secret storage with secure erase.
- `forge.crypto.core.secret_string`: copyable secret text with explicit scrubbed
  copy/move assignment and destruction.
- `forge.crypto.core.random`: cryptographically secure random bytes.
- `forge.crypto.core.constant_time`: constant-time equality for equal-length
  byte sequences at authentication boundaries.

```cpp
import forge.crypto.core.random;
import forge.crypto.core.secret_bytes;

auto secret = forge::crypto::core::secret_bytes{
   forge::crypto::core::random_bytes(32)};
```

The target depends on `forge_exceptions` and OpenSSL Crypto. It does not own
hash, cipher, key or certificate algorithms. `test_forge_crypto_core` covers
secret lifetime, random byte and constant-time comparison contracts; the
package test verifies installed module consumption.
