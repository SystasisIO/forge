# Forge Pnet

`forge_net_pnet` owns a product-neutral pre-shared-key stream protector. A
`pre_shared_key` decodes the canonical libp2p swarm-key base16 form, owns its
32-byte value as move-only secret material, and exposes two distinct fingerprints,
never the raw key. The existing `fingerprint()` returns the domain-separated
32-byte `operational_fingerprint`: SHA-256 over
`"forge.net.pnet.operational-fingerprint.v1" || 0x00 || decoded PSK`.

`pre_shared_key::network_fingerprint()` and `protector::network_fingerprint()`
return the separate 16-byte `network_fingerprint` value, declared by
`forge.net.pnet.network_fingerprint`. This matches the Rust/Go libp2p
fingerprint: Salsa20 with the decoded 32-byte key, nonce ASCII `finprint`,
counter zero and 64 bytes of keystream, followed by SHAKE128 with 16-byte output.
PNet composes only Forge symmetric/digest wrappers and erases the temporary key,
keystream and digest buffers on scope exit, including exceptions.

The existing operational fingerprint, diagnostics and stream protocol are
unchanged. These values are identifiers, not proofs of key possession or peer
authentication; neither makes a weak key safe, and logging them can correlate
networks. Moved-from key/protector access throws `exceptions::invalid_options`.
No discovery service or mDNS runtime is introduced.

```cpp
import forge.net.pnet.network_fingerprint;
import forge.net.pnet.protector;

// swarm_key_text is secret configuration supplied by the application.
const auto key = forge::net::pnet::pre_shared_key::parse_swarm_key(swarm_key_text);
const auto operational = key.fingerprint();
const auto network = key.network_fingerprint();
```

`test_forge_net_pnet` preserves the operational SHA256 golden and adds the
Rust donor `transports/pnet/src/lib.rs` network golden
`45fc986bbc9388a11d939df26f730f0c`, key/protector moves and invalid-key checks.
The existing `net_pnet` package consumer covers both accessors; no package
component or target is added.

`protector::async_protect` eagerly writes one local 24-byte nonce, then returns
a stream whose peer nonce is read lazily. Read and write directions use
independent raw XSalsa20 streams. The primitive adds no KDF, MAC, frame, or
protocol ID. It protects a stream but does not authenticate a peer; a wrong PSK
normally becomes visible only during the following security negotiation.

The optional stop token covers the eager nonce write. Cancellation requests the
lower transport exactly once; if the nonce write wins the terminal race, the
protected connection is returned normally.

## Interop Evidence

`tests/libp2p_interop` registers four direct Forge-to-Go/Rust and Go/Rust-to-
Forge TCP/Yamux directions using a canonical `swarm.key` source fixture outside
the runner artifact directory. Both endpoint records must confirm PNET
negotiation and the non-secret Forge operational fingerprint. Separate missing-
key and mismatched-key controls require observed listener ingress followed by
rejection before Identify or an application stream. Registration is not a passing live-run claim, and the
evidence makes no QUIC, Relay, or DCUtR claim.
