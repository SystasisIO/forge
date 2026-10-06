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
| `go-libp2p/p2p/net/reuseport/dial.go` | Ordinary TCP dialing selects a compatible listening socket; coordinated dialing additionally preserves the agreed security role | Existing TCP profile and listener owner |
| `go-libp2p/p2p/net/reuseport/reuseport.go`, `reuseport_posix.go` | An ordinary reused tuple may be busy or in TIME_WAIT; a bounded fallback uses a different native source port without renewing the logical deadline | TCP preferred reuse policy; coordinated reuse remains mandatory |
| `go-libp2p/p2p/host/basic/addrs_manager.go` | Hole-punch candidates include authenticated observations with one observer, separately from the stronger public-advertisement quorum | Existing observed-address manager |
| `rust-libp2p/protocols/dcutr/src/behaviour.rs` | Per-peer coordination; candidate readiness and direct-connection completion | Path manager and authenticated session publication |
| `rust-libp2p/protocols/dcutr/src/behaviour.rs`, `on_dial_failure` and `InboundConnectNegotiated` | The inbound-exchange dial is not entered in `outgoing_direct_connection_attempts`; its failed native wave can lack an aggregate DCUtR error event | Explicit Rust-source negative-evidence limitation, not a Forge lifecycle exception |
| `rust-libp2p/protocols/stream/src/shared.rs` | The pinned generic stream control randomly selects an existing connection, rather than preferring a direct path | Rust fixture limitation; Forge direct preference has separate native regressions |
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

An observed external address is useful only when the underlying socket is
owned by the listening transport. Ordinary relay dialing therefore also needs
compatible listener reuse; an observation of a separate ephemeral socket must
not be rewritten to the listening port. Private hole-punch candidate selection
does not lower the confirmation threshold used for signed public addresses.

TLS `libp2p` ALPN and an absent legacy Noise muxer extension retain multistream
fallback. Unknown or unauthenticated extension data is not a negotiated fact.
Go inline success and pinned Rust fallback require different actual receipts.

Private PSK remains TCP/Yamux-only. Port reuse and application protocols may be
proved in that profile, but neither Relay nor DCUtR becomes permitted. The eight
private protocol gaps require independent exchanges and wrong/missing-key
negative controls, not inference from the common-secret handshake.

The pinned Rust generic stream control cannot select a particular connection.
For the initial relay application stream, the fixture uses the public native
`NotifyHandler::One` operation on the exact existing inner relay connection.
This outbound-only application behaviour cannot dial or trigger DCUtR; it
captures the actual muxer stream and has a bounded admission-to-negotiation
deadline. Its trace identifies the distinct application component, not generic
stream control. Cancellation of its receiver does not release native capacity
until callback or handler destruction. No DCUtR wire timing or roles are gated.
Its successful path fixture therefore retires only the original inner relay
connection, using the public Swarm close operation, after authenticated native
DCUtR success and completed relay-before application I/O. The fixture must await
the actual ConnectionClosed event, preserve the outer relay carrier, verify the
successful direct connection remains live, and then open a fresh application
stream without another dial. This proves direct delivery after explicit native
retirement; it does not prove Rust automatically prefers direct streams while
both paths remain live. Failed and canceled cases must never retire the original
relay connection and must reuse the retained application stream.

For the pinned Rust source role, an inbound CONNECT/SYNC exchange can launch a
native dial whose failure does not produce an aggregate DCUtR terminal event.
The negative fixture must bind the unchanged native behaviour's actual
`ToSwarm::Dial` to its typed `OutgoingConnectionError`, using the same connection
ID, peer, exchanged addresses and native stream receipts. Its explicit scope is
`native_wave_failure`: it proves failure of that wave, not completion or drain
of the Rust behaviour. Forge must independently reach its failed terminal state
or accept and join cancellation, and the original relay application stream must
still transfer a fresh challenge before host shutdown. This exception does not
apply to Go or the Rust destination role and cannot synthesize a Rust DCUtR
completion event.

## Evidence Gate

Acceptance must bind clean HEAD, native binary provenance, actual wire exchanges,
authenticated peer identities, transport endpoints and gracefully joined
processes. Required socket evidence compares the actual local address/port with
the listener owner. Required path evidence includes direct application delivery
after CONNECT/SYNC and relay delivery after a failed upgrade. Decoder unit tests
and configured protocol labels do not replace these observations.
