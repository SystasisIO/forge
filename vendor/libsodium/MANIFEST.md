# libsodium 1.0.22 Vendor Manifest

Forge vendors the official libsodium 1.0.22 release tree as a private XSalsa20/Salsa20
backend for `forge_crypto_symmetric`. Libsodium headers, types and symbols are
not part of the Forge public C++ API.

## Source Pin

- Version: `1.0.22`
- Release archive: `https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz`
- Archive SHA-256: `adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349`
- Upstream release tree: `vendor/libsodium/upstream`

`upstream/` is an unmodified extraction of the official release archive. Forge
does not apply patches to that tree.

## Complete Release Tree

`TREE.SHA256` is the complete 678-file full manifest for `upstream/`. Its SHA-256 is
`b7a7f9f72927a2bcd47ca29822c7c35902dbe4c128bfefd73eb67028d69338bc`.
The structure gate rejects any missing, extra, or changed upstream file.

The private target applies the compile-time symbol prefix
`forge_xsalsa20_private_` to every selected upstream `crypto_core_*`,
`crypto_stream_*`, `randombytes_buf`, and `sodium_memzero` symbol. The
Forge-prefixed bridge names are not published through a Forge API and are checked
only by the private test boundary.

## Compiled Upstream Subset

The private object target compiles only the portable XSalsa20/Salsa20 path below. It
does not configure or expose a general libsodium installation.

| File | SHA-256 |
| --- | --- |
| `upstream/src/libsodium/crypto_stream/xsalsa20/stream_xsalsa20.c` | `0c3d8b9b45ee606ac9b7a4b651347e72b2f629e6d17e513c7a7aa161058c4aec` |
| `upstream/src/libsodium/crypto_stream/salsa20/ref/salsa20_ref.c` | `e47d9a29430d6ee4d2f7789e309984bf98997b3e5e7665f4efb006b705d50fc2` |
| `upstream/src/libsodium/crypto_core/salsa/ref/core_salsa_ref.c` | `140bc08136a05e0d57aa829a5b49969d72a826602a2cbd4f0e0589f195750b4c` |
| `upstream/src/libsodium/crypto_core/hsalsa20/core_hsalsa20.c` | `ecb3514e479cc993366542f9efe71de9aafd8f439cf9223f4ab0d97e9b97625a` |
| `upstream/src/libsodium/crypto_core/hsalsa20/ref2/core_hsalsa20_ref2.c` | `d9fdd18eeaaf6964ecb30eee30314a32abd10360c660af253cdab83826026a13` |
| `upstream/src/libsodium/include/sodium/crypto_stream_xsalsa20.h` | `e543c52f1f0c2f381566fd9800c1d67b24bd64322cea6cd49ef458b695ba27d7` |
| `upstream/LICENSE` | `508a76d186356c0dd807a670ef510964f8724557024796a2c426c6c0e19ab683` |

`integration/xsalsa20_support.c` is Forge-owned build glue, outside the
unmodified upstream tree. It supplies the upstream portable implementation's
secure-erase fallback, binds the selected reference Salsa20 implementation and
makes internal XSalsa20 and Salsa20 keystream calls available to Forge C++ code. It does
not publish a libsodium API or key-generation surface.
