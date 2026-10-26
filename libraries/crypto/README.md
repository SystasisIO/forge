# Forge Crypto Family

`forge.crypto` is a family of focused cryptographic libraries. The directory
and namespace are grouping boundaries only: there is no family-wide target,
package component or aggregate module.

## Libraries

| Library | Target | Component | Responsibility |
| --- | --- | --- | --- |
| [core](core/README.md) | `forge_crypto_core` | `crypto_core` | Owned bytes, secret bytes, secure erase and random data. |
| [digest](digest/README.md) | `forge_crypto_digest` | `crypto_digest` | Hashes, HMAC and Raw pack hashing. |
| [exchange](exchange/README.md) | `forge_crypto_exchange` | `crypto_exchange` | Experimental bounded exchange of authenticated source openings, shared native/WASM checker. |
| [symmetric](symmetric/README.md) | `forge_crypto_symmetric` | `crypto_symmetric` | AES, ChaCha20-Poly1305, HKDF and scrypt. |
| [asymmetric](asymmetric/README.md) | `forge_crypto_asymmetric_values`, `forge_crypto_asymmetric` | `crypto_asymmetric_values`, `crypto_asymmetric` | Binary key/signature values and host signing algorithms. |
| [signer](signer/README.md) | `forge_crypto_signer` | `crypto_signer` | Product-neutral private-key provider interface. |
| [keystore](keystore/README.md) | `forge_crypto_keystore` | `crypto_keystore` | Encrypted local-file signer provider and password input boundary. |
| [pki](pki/README.md) | `forge_crypto_pki` | `crypto_pki` | DER, PEM and X.509 boundaries. |
| [math](math/README.md) | `forge_crypto_math` | `crypto_math` | Big integers and modular arithmetic. |
| [bls](bls/README.md) | `forge_crypto_bls` | `crypto_bls` | BLS values, signatures and contract primitives. |
| [bn256](bn256/README.md) | `forge_crypto_bn256` | `crypto_bn256` | BN254 add, multiply and pairing operations. |

Base32 is a general encoding and lives in
[`forge_codec_base32`](../codec/base32/README.md). CityHash was removed because
Forge had no first-party consumer or persisted contract for it.

### CityHash Migration

The removed `forge.crypto.city` module and `forge::crypto::city_hash*`
functions have no drop-in Forge replacement. CityHash is a non-cryptographic
hash, so consumers that own an existing CityHash-based wire or persisted
contract must keep a CityHash implementation at that compatibility boundary.
New Forge code should select a digest from `forge_crypto_digest` and treat the
resulting bytes as a new format rather than silently replacing stored CityHash
values. The complete source/package migration is documented in the
[Forge 8.12.0 release notes](../../docs/releases/8.12.0.md).

## Consumption

Request and link only the leaf that owns the required API:

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS crypto_digest crypto_asymmetric_values)
target_link_libraries(app PRIVATE
   Forge::forge_crypto_digest
   Forge::forge_crypto_asymmetric_values
)
```

```cpp
import forge.crypto.digest.sha256;
import forge.crypto.asymmetric.values;
```

The lightweight values component exports binary `algorithm`, `public_key` and
`signature` types without pulling the host signing implementation. Text key and
signature encoding is an explicit boundary in `crypto_asymmetric`.

## Boundaries

- Cryptographic primitives are synchronous and own no Asio runtime policy.
- `signer` and `keystore` are custody/provider leaves rather than primitives;
  their awaitable provider boundary permits HSM, KMS and hardware-backed
  implementations without putting transport policy into asymmetric crypto.
- OpenSSL remains the host backend where required; K1 compatibility keeps its
  dedicated implementation.
- Raw and Variant byte layouts, algorithm enum values, signatures and keys are
  compatibility surfaces and are tested byte-for-byte.
- `forge::crypto` owns no public declarations directly. Public symbols belong
  to `core`, `digest`, `symmetric`, `asymmetric`, `signer`, `keystore`, `pki`,
  `math`, `bls` or `bn256`.
