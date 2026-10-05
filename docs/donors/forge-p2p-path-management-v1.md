# Forge P2P Path Management v1

## Baseline And Scope

Stage 6 PR10 uses the pinned Go `9cfe2cc0`, Rust `22fb4c78`, and specifications
`6b6203ee` checkouts. This note records inspected patterns and acceptance
requirements, not an interoperability verdict. Stage 7 configuration and Stage 8
production promotion remain separate.

| Donor Source | Accepted Pattern | Forge Owner |
| --- | --- | --- |
| `libp2p-specs/relay/DCUtR.md` | Inbound relay side initiates; CONNECT/SYNC, measured RTT, coordinated transport roles, authenticated direct upgrade | Node path manager and existing DCUtR codec |
| `go-libp2p/p2p/protocol/holepunch/holepuncher.go` | Wait for Identify, try unilateral direct dial first, fresh exchange per bounded retry, one attempt per peer, joined shutdown | Node-owned path operation |
| `go-libp2p/p2p/protocol/holepunch/svc.go` | Reject direct DCUtR streams; filter candidates and admit bounded decoding | Existing admitted P2P stream and resource reservations |
| `go-libp2p/p2p/transport/tcp/tcp.go`, `rust-libp2p/transports/tcp/src/lib.rs` | Explicit local socket binding and coordinated security role | Existing TCP connector/listener |
| `go-libp2p/p2p/transport/quic/transport.go` | Listener-owned UDP reuse and responder hole-punch traffic | Existing QUIC engine |
| `rust-libp2p/protocols/dcutr/src/behaviour.rs` | Per-peer coordination; candidate readiness and direct-connection completion | Path manager and authenticated session publication |
| `libp2p-specs/connections/inlined-muxer-negotiation.md` | TLS ALPN selection; Noise chooses the first common initiator preference | Existing security upgrade |
| `go-libp2p/p2p/security/noise/transport.go` | Bounded early-data muxer list and identity verification before publishing connection facts | Existing Noise handshake |
| `rust-libp2p/transports/tls/src/lib.rs`, `rust-libp2p/transports/noise/src/io/handshake.rs` | Pinned fixed-ALPN TLS and absent Noise early muxer extension use fallback | Existing multistream negotiation |

## Composition And Rejected Shortcuts

The path manager owns admission, coalescing, retry history and cancellation. It
does not create another transport, peer store, discovery loop or runtime. Existing
Forge lifecycle tracking, notifications, deadlines, gaters and resource ownership
remain the mechanics. Relay transport and AutoRelay retain their existing owners.

A relay-side automatic trigger waits for Identify on that authenticated session.
DCUtR candidates are bounded, canonical direct addresses and bind to the expected
peer. Circuit addresses are never direct-dial candidates. An authenticated direct
session, not an open socket, establishes success; new streams prefer it without
invalidating existing relay streams. Failure retains the usable relay path.

TCP security-server role on an outgoing simultaneous-open socket must not be
confused with transport dial direction. QUIC responder traffic must use the
listener socket; a second ordinary QUIC client dial is not equivalent. A silent
ephemeral-port fallback cannot establish a required port-reuse proof.

TLS `libp2p` ALPN and an absent legacy Noise muxer extension retain multistream
fallback. Unknown or unauthenticated extension data is not a negotiated fact.
Go inline success and pinned Rust fallback require different actual receipts.

Private PSK remains TCP/Yamux-only. Port reuse and application protocols may be
proved in that profile, but neither Relay nor DCUtR becomes permitted. The eight
private protocol gaps require independent exchanges and wrong/missing-key
negative controls, not inference from the common-secret handshake.

## Evidence Gate

Acceptance must bind clean HEAD, native binary provenance, actual wire exchanges,
authenticated peer identities, transport endpoints and gracefully joined
processes. Required socket evidence compares the actual local address/port with
the listener owner. Required path evidence includes direct application delivery
after CONNECT/SYNC and relay delivery after a failed upgrade. Decoder unit tests
and configured protocol labels do not replace these observations.
