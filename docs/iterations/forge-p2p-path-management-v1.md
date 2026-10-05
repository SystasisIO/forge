# Forge P2P Stage 6 PR10: Path Management

## Scope

Deliver node-owned direct upgrades of authenticated relay connections, coordinated
TCP/QUIC dialing, and TLS/Noise inlined Yamux negotiation. Complete the eight
private-profile acceptance contracts retained since PR4 in the same PR.

The baseline is the Stage 6 production implementation roadmap and its donor-first
capability manifest. Donors remain the commits pinned in `fixture-lock.json`:
Go libp2p `9cfe2cc0`, Rust libp2p `22fb4c78`, and specifications `6b6203ee`.
Stage 7 plugin configuration and Stage 8 production promotion remain separate.
The private PSK profile continues to prohibit QUIC, Relay, and DCUtR.

## Ownership And Composition

- One node-owned per-peer upgrade operation coalesces automatic and manual
  triggers. State, workers, retries, deadlines, cancellation, and shutdown are
  bounded and owned by the existing node lifecycle.
- The inbound authenticated relay connection triggers Identify and then a
  unilateral direct attempt before DCUtR, following the donor role selection.
- DCUtR uses the existing codec, admitted streams, transport registry, connection
  gater, resource reservations, and authenticated session publication.
- Every DCUtR retry performs a fresh CONNECT/SYNC exchange and RTT measurement.
  Direct candidates are filtered and bounded before dialing. Ineligible relay
  addresses and mismatched peer identities cannot become direct candidates.
- Coordinated TCP dialing preserves security/muxer client and server roles.
  QUIC uses the listener's UDP ownership and donor-compatible hole-punch roles.
  Ordinary dialing and coordinated dialing share their transport owner.
- A direct connection must authenticate the expected peer before success.
  Existing relay streams retain their owner and remain usable on upgrade failure;
  new streams prefer the verified direct connection.
- Inlined muxer negotiation belongs to the existing TLS/Noise transport upgrade.
  Go's inline behavior and the pinned Rust fallback are distinct evidence.
- Forge notifications, deadlines, cancellation, and tracked tasks are reused.
  Public `hole_punch::attempt` bookkeeping is replaced by real private ownership.

## Acceptance Contracts

PR10 owns eleven path/negotiation contracts already declared by the manifest:
DCUtR; native/private coordinated port reuse; Go TLS/Noise inline negotiation in
native/private profiles; Rust Noise and fixed-ALPN TLS fallback in both profiles.

The additional private-profile contracts are:

1. `tcp_yamux_private_pnet`.
2. `multistream_select_private_pnet`.
3. `noise_identity_private_pnet`.
4. `tls_identity_private_pnet`.
5. `ping_private_tcp_yamux_pnet`.
6. `identify_private_tcp_yamux_pnet`.
7. `kademlia_amino_private_tcp_yamux_pnet`.
8. `rendezvous_rust_private_tcp_yamux_pnet`.

Each contract needs an executable semantic validator and observed endpoint
evidence, with negative controls. A configured protocol, generic echo, source
label, or `status=ok` alone does not establish protocol-specific acceptance.
Supported bilateral directions follow the pinned donor implementations;
Rendezvous does not acquire an unsupported Go claim.

## Validation And Delivery

- Unit and raw-node regressions cover coalescing, retry exhaustion, malformed
  exchanges, authentication mismatch, relay fallback, resource bounds,
  cancellation, transport ownership, and deterministic shutdown.
- Live Go/Rust exchanges observe the relay path, negotiated DCUtR exchange,
  actual authenticated direct transport, and subsequent application delivery.
  Failed upgrades prove that the existing relay path still transfers data.
- TLS/Noise tests distinguish selected inline Yamux from legacy multistream
  fallback and reject malformed or unauthenticated negotiation facts.
- Private tests exercise matching, missing, and mismatched keys alongside the
  exact requested protocol and its observable result.
- Focused package, structure, inventory, fixture-lock, and affected transport
  suites run in one coordinator-owned build with `-j4`. Full CI is not run.
- Both independent source/evidence reviews must be clean on the submitted
  commit. Remaining Stage 6 PR11/PR12 contracts remain explicitly unpromoted.
- After reviewed acceptance, merge into `dev` and prepare PR11 from fresh
  `origin/dev`. Preserve indexed evidence before deleting this task's generated
  build resources; unrelated worktrees and active builds are retained.

## Coordination

Developers and reviewers use Sol 6.1 with xHigh reasoning and bounded prompts.
Transport negotiation and acceptance fixtures have disjoint write scopes.
The coordinator owns shared integration, build execution, and review arbitration.
This document defines work and acceptance; it does not claim implementation or
production readiness.
