# Bounded source openings: donor traceability

Date: 2026-10-25. Status: accepted for restricted local engineering tests only;
production/commercial qualification remains open. See the
[qualification report](../validation/crypto-exchange-2026-10-25/README.md).

Primary references inspected:

- TLSNotary `5f67389de3f8962227fddffd71d78a852acc9b01`,
  `crates/core/src/transcript/hash.rs` and
  `crates/tlsn/src/transcript_internal/commit/hash.rs`: exact
  SHA256(selected plaintext || random 16-byte blinder). Existing Forge JOB-3
  checks openings through the managed upstream worker. Metadata is outside the
  hash and needs trusted verifier storage or whole-record/context authentication.
- [FairSwap](https://eprint.iacr.org/2018/740), Dziembowski/Eckey/Faust, CCS 2018;
  [author PoC](https://github.com/lEthDev/FairSwap/blob/07f5664de64e232a0e1a8c0a84eefc223b45ae8b/FairFileSale.sol),
  `cryptLarge` hash pad and authenticated fault pattern. The PoC's README says
  it has not been audited for implementation bugs. No PoC code is copied here.
- Lisa Eckey's primary author exposition, section 5.4.1 Algorithms 6–8,
  [dissertation](https://tuprints.ulb.tu-darmstadt.de/server/api/core/bitstreams/0d999e6b-864a-4470-9a6a-984db287b388/content),
  explains authenticated encrypted wires and objective faulty computation.

Accepted restricted composition: split ONE direct raw response body into at most
16 full-coverage blocks, 2048 bytes except the last. Independently authenticated
MPC digests for every plaintext+blinder become the expected source hashes. The
buyer validates exact ordered ranges, source origin, visible request/headers and
complete ciphertext before authorizing an offer. A signed direct digest list
replaces Merkle membership for this bounded list; no generic circuit is claimed.
A wrong delivered body must yield at least one different source opening, modulo
SHA-256 collision resistance. Its committed ciphertext is bounded and can be
presented, decrypted and checked by the contract after the key is revealed.

Cipher pattern: Keccak-256(BE256(counter) || key32) XOR each 32-byte word, using
counter=65*block_index+word_index. A 2048-byte plaintext plus 16-byte blinder pads
to 2080 bytes=65 words. Indices 0..15 and words 0..64 are injective; restarting
counters at each block is forbidden. Existing native SHA3(false) and guest Keccak
supply the primitive. A fresh secret key is mandatory for each offer; this
implementation has no key service or cross-offer key-reuse detector.

Rejected: seller-selected fileRoot as a substitute for MPC source commitments;
buyer signature over an opaque root; treating a saved evidence blob as an offline
TLS proof; claiming that generic C++/CEL code is an authenticated FairSwap circuit;
whole-transcript disclosure; API-key forwarding; copying TLS/crypto/VM machinery;
unbounded file/HTTP/JSON processing in the contract; automatic production enable.

Current scope excludes async task/locator links, arbitrary HTTP framing, JSON/base64
artifact extraction, compression/redirects, arbitrary large images, Labs/OpenAI,
GPU proofs, production policy, token settlement and economic viability. These
need separately accepted profiles and tests, not an implicit widening of this one.
Public disputes reveal their block. Even failed reveal transactions expose the
key, so finality/inclusion and post-reveal payment liveness remain external gates.

Owning implementation: `libraries/crypto/exchange`, shared native/WASM component.
Consumer policy: Turbine's test-deployment profile; no product identity in Forge.
Tests: independently computed PyCryptodome Keccak vectors at word counters
0/64/65/1039; native/WASM canonical bytes and fault results; hostile bounds,
padding and foreign witnesses; real local HTTPS MPC with full source coverage and
negative substitution before the buyer's authorization gate. No Linux/real Labs.
