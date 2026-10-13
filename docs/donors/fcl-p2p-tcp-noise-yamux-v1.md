# FCL P2P TCP + Noise + Yamux Traceability

This note is proof traceability for Block E.2a. It is not a second roadmap; the
canonical block order remains `docs/network/quic-p2p.md`.

## Scope

Supported in this slice:

- direct `/tcp/.../p2p/<peer>` dial/listen;
- libp2p multistream-select security negotiation for Noise;
- libp2p Noise Peer ID verification and identity payload handling;
- `/yamux/1.0.0` muxer selection;
- reusable `fcl_yamux` session as the resulting `transport::session`;
- FCL <-> go-libp2p and FCL <-> rust-libp2p Ping, Identify and framed echo.

Deferred in E.2a:

- the TLS security branch for libp2p TCP, now closed by E.2b and tracked in
  `docs/donors/fcl-p2p-tcp-tls-yamux-v1.md`;
- WebSocket transport paths `/ws` and `/wss`;
- product/API integration above P2P.

## Donors Inspected

| Area | Donor files | Accepted pattern |
| --- | --- | --- |
| TCP transport layering | `donors/go-libp2p/core/transport/transport.go`, `donors/go-libp2p/p2p/transport/tcp/tcp.go`, `donors/rust-libp2p/transports/tcp/src/provider/tokio.rs` | TCP is a raw transport below security and mux upgrade. |
| Upgrade pipeline | `donors/go-libp2p/p2p/net/upgrader/upgrader.go`, `donors/go-libp2p/p2p/net/upgrader/conn.go`, `donors/rust-libp2p/libp2p/src/builder.rs` | Transport -> security -> mux -> swarm/session layering. |
| Security negotiation | `donors/go-libp2p/p2p/test/negotiation/security_test.go`, `donors/go-libp2p/p2p/security/noise/handshake.go`, `donors/rust-libp2p/interop-tests/src/arch.rs` | Noise is negotiated as the TCP security protocol and verifies the remote peer key. |
| Yamux negotiation | `donors/go-libp2p/p2p/test/negotiation/muxer_test.go`, `donors/go-libp2p/p2p/muxer/yamux`, `donors/rust-libp2p/muxers/yamux` | `/yamux/1.0.0` is the TCP stream multiplexer. |
| Live acceptance | `donors/go-libp2p/test-plans/cmd/ping/main.go`, `donors/rust-libp2p/examples/ping/src/main.rs`, `donors/rust-libp2p/interop-tests/src/arch.rs` | Both Go and Rust expose TCP+Noise+Yamux as normal libp2p direct transport composition. |

## FCL Coverage

| Behavior | FCL component test | Live interop scenario |
| --- | --- | --- |
| FCL TCP listener/dialer establishes direct P2P session through Noise and Yamux | `test_fcl_libp2p_interop tcp ping/identify/echo FCL <-> Go/Rust` | FCL listener <-> Go/Rust dialer `tcp ping`, `tcp identify`, `tcp echo`; Go/Rust listener <-> FCL dialer same scenarios |
| Peer mismatch is rejected as typed P2P failure | `test_fcl_quic_p2p p2p_direct_tcp_rejects_tls_peer_mismatch` covers the TCP direct path verification contract; Noise fallback is covered by live TCP fixtures | Component-level mismatch and live valid identities |
| `/ws` and `/wss` are parse/store only | `test_fcl_quic_p2p p2p_websocket_multiaddr_is_parseable_but_not_dialable` | No live dial/listen scenario |
| Relay/DCUtR continue to use reusable Yamux without changing wire behavior | Existing relay/DCUtR component and live interop scenarios | Existing Relay v2, relayed stream and DCUtR live matrix |

## Notes

### Explicit Stream Security Policy

Forge's additive `node::stream_security` policy maps to these pinned donors:

- Go `9cfe2cc00be5b20a0be737f002c99f81b92255c5`: `options.go:Security`
  appends the enabled security transports; `p2p/net/upgrader/upgrader.go:New`
  preserves their protocol order and `negotiateSecurity` selects from that list.
  `setupSecurity` runs only the selected handshake, not a second security
  protocol after handshake failure.
- Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `libp2p/src/builder/phase/tcp.rs:with_tcp` accepts a singleton security upgrade
  or an ordered tuple. `libp2p/src/builder/select_security.rs:SelectSecurityUpgrade`
  concatenates protocol offers in preference order and delegates the selected
  inbound/outbound handshake without retrying another upgrade on failure.

`tls_and_noise` retains Forge's existing TLS-first order; `tls` and `noise`
are singleton allowlists. The same private `stream_upgrade` helper owns ordinary
TCP and PNET negotiation in both security roles. Neither PNET nor physical TCP
direction changes the configured allowlist, expected Peer ID or Yamux role.
QUIC TLS is independent. Relay endpoint upgrades remain Noise-only, so TLS-only
relay clients are rejected while service-only circuit forwarding is allowed.

`tests/quic_p2p/stream_security_tests.cpp` exercises native authentication,
mixed/default/singleton compatibility, disjoint rejection, PNET, expected-peer
rejection, reversed native upgrade roles, public node propagation and unchanged
QUIC TLS. It is registered in the existing aggregate and path-management test
targets. Source additions are not a native or donor-live acceptance claim.

- `fcl_tcp` remains raw TCP and does not know Peer ID, Noise, multistream-select
  or Yamux.
- `fcl_yamux` remains reusable mux mechanics and does not know P2P identity or
  protocol negotiation.
- `fcl_p2p` owns the libp2p-specific security payload, Peer ID verification,
  multistream-select protocol choice and direct path policy.
