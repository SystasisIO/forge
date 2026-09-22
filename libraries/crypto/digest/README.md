# Forge Crypto Digest

`forge_crypto_digest` owns deterministic digest algorithms, HMAC and Raw pack
hashing. Package component: `crypto_digest`. Public namespace:
`forge::crypto::digest`.

## Modules

The leaf exports SHA-1, SHA-224, SHA-256, SHA-3, SHA-512, RIPEMD-160, BLAKE2,
`forge.crypto.digest.hmac` and `forge.crypto.digest.packhash`.
The `forge.crypto.digest.shake128` module adds variable-output SHAKE128.

```cpp
#include <string>

import forge.crypto.digest.sha256;

const auto value = forge::crypto::digest::sha256::hash(
   std::string{"canonical bytes"});
```

`data()` and `to_uint8_span()` return borrowed views and are available only on
lvalues. Their rvalue overloads are deleted, so a view cannot be obtained from
a temporary digest. Keep the digest in a named object for the full lifetime of
the pointer or span.

Dependencies are `forge_crypto_core`, Codec Hex, Raw, Variant, Forge Core and
OpenSSL Crypto. Digest values preserve their existing Raw and Variant layouts.
The library does not own text transport profiles or signing policy.
`test_forge_crypto_digest` covers golden vectors, incremental encoders, HMAC,
BLAKE2, serialization and compiler-enforced borrowed-view lifetimes.

## SHAKE128

`forge::crypto::digest::shake128(input, output_size)` accepts a byte span and
returns an owning byte vector. The size is in bytes, not bits; zero returns an
empty vector. This is an extendable-output function (XOF), not SHA3-256, and
does not change existing digest values or serialization.

```cpp
#include <array>
#include <cstdint>
import forge.crypto.digest.shake128;

const auto input = std::array<std::uint8_t, 3>{'a', 'b', 'c'};
const auto fingerprint = forge::crypto::digest::shake128(input, 16);
```

OpenSSL EVP is private to the implementation; finalization uses
`EVP_DigestFinalXOF`. Requests beyond vector capacity throw
`digest::exceptions::invalid_size`, backend failures throw
`digest::exceptions::backend_error`, and allocation failure remains
`std::bad_alloc`. Bound untrusted output sizes before allocating. Output length
is protocol policy; do not treat arbitrary short outputs as collision-resistant
identifiers or authentication. Erase output if the application treats it as
secret. Native tests cover empty/`abc` goldens and zero/rate-boundary lengths;
the existing `crypto_digest` package consumer imports this module directly.
