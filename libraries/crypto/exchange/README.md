# Bounded opening exchange

Experimental native/WASM mechanism for a fixed direct-source profile. This is
not a generic FairSwap circuit, TLS verifier, signer, financial protocol or a
production-ready Labs adapter. Its construction/assumptions are recorded in
[the donor note](../../../docs/donors/forge-crypto-exchange-blocks-v1.md).

An immutable manifest binds an application context, authenticated evidence,
fresh one-time key hash and up to 16 ordered blocks. Each block records its
plaintext length, independently authenticated source SHA-256 and ciphertext
SHA-256. All but the last block contain exactly 2048 plaintext bytes. An opening
is those bytes followed by the upstream 16-byte blinder; its source commitment
is SHA256(plaintext || blinder). An offer is not authenticated by calling
`validate`: the application must compare every source hash/order/size/range
against the verifier's trusted MPC result before signing the complete offer.

Encryption follows the FairSwap paper/PoC hash-pad pattern using the existing
Forge Keccak-256 implementations. Word j of block i uses counter 65*i+j, encoded
as a big-endian 256-bit integer, followed by the 32-byte one-time key. XOR the
32-byte Keccak pad with each opening word; zero-pad the last word. No new hash,
random generator, TLS, VM intrinsic or runtime is introduced. The caller MUST
provide a cryptographically fresh, secret key for each offer and preserve it
with the ciphertext; reusing a key leaks XOR relationships. Fixed test keys are
only test vectors. This module does not claim to detect caller key reuse.

The direct list authenticates each ciphertext by index, avoiding Merkle paths
for this bounded profile. `proves_fault` first checks the manifest/key/index and
the supplied ciphertext hash; malformed/foreign witnesses raise `invalid_input`.
A genuine committed block with incorrect length, padding or source hash proves
invalid delivery. `open` returns only valid plaintext or raises
`invalid_delivery`. A correct block does not prove a fault. Work is bounded by
16 metadata entries and 65 Keccak words for one opening; maximum ciphertext is
2080 bytes. Application action limits must be checked before Raw decoding.

The bytes revealed in a public dispute are public, including after rejection.
Credential/prompt regions MUST NOT be sold as result openings. Evidence origin,
HTTP framing/model/task semantics, complete artifact coverage, durable download,
key custody, chain finality and payment authorization belong to the consumer.

Native dependencies: existing crypto core/digest, Raw, typed exceptions.
Guest uses the same physical declarations/implementation, canonical Raw, existing
SHA256 and Keccak intrinsics and guest policy errors. SDK component
`forge.crypto.exchange` exports `Forge::forge_crypto_exchange`; crypto core exposes
only its existing bytes value module to the guest. Native archives/BMIs are not
reused by WASM. No protocol fields are accepted through ad-hoc JSON.

Validation is reported independently with exact source hashes, native/WASM
vectors, real MPC composition, negative witnesses and resource measurements.
