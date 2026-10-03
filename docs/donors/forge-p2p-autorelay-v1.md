# Forge P2P AutoRelay v1

## Scope And Baseline

Stage 6 PR9 completes node-owned AutoRelay and Circuit Relay v2 service ownership.
It does not implement DCUtR automation, UPnP or plugin configuration. P2P remains
Preview; this change is not the Stage 8 production acceptance gate.

Reviewed donor baselines are Go libp2p `9cfe2cc0`, Rust libp2p `22fb4c78` and
libp2p specifications `6b6203ee`, as pinned in the interop fixture lock.

| Donor Source | Accepted Pattern | Forge Owner |
| --- | --- | --- |
| `go-libp2p/p2p/host/autorelay/autorelay.go` | Unknown/private reachability starts automatic relay acquisition; public reachability stops it | `detail::autorelay_manager`, node reachability integration |
| `go-libp2p/p2p/host/autorelay/relay_finder.go` | Authenticate and Identify relay candidates, bound attempts, renew leases, back off failures, stop and join workers | AutoRelay manager and existing HOP exchange |
| `go-libp2p/p2p/protocol/circuitv2/client/reservation.go` | Check expiration, returned addresses and any supplied signed voucher before accepting a reservation | Node reservation aspect and existing signed-envelope codec |
| `go-libp2p/p2p/protocol/circuitv2/relay/relay.go` | Explicit relay service, connected destination, admission and scoped circuit ownership | Node relay service and existing resource manager |
| `rust-libp2p/protocols/relay/src/protocol/outbound_hop.rs` | Renewal timing derives from the accepted reservation, not topology refresh | AutoRelay scheduler |
| `rust-libp2p/protocols/relay/src/priv_client.rs` | Circuit addresses and reservation validity depend on the relay connection | Node-owned reservation and advertised-address state |
| `rust-libp2p/protocols/relay/src/behaviour.rs` | Bounded relay service reservations/circuits with independent resource limits | Relay policy and resource admission |
| `libp2p-specs/relay/circuit-v2.md` | HOP/STOP protobuf and protocol IDs, expiry, advisory vouchers, unlimited zero/absent remote limits | Existing relay codec and reservation validation |
| `go-libp2p/p2p/host/basic/basic_host.go`, `rust-libp2p/swarm/src/handler/select.rs` | Explicit protocol routing is honored instead of being silently shadowed by built-in dispatch | Shared registered-handler dispatch after authentication and resource admission |

## Composition

Topology remains the only autonomous discovery owner. AutoRelay consumes bounded
candidate hints and verifies the authenticated direct session and exact HOP
support before reservation. A recovered cache capability alone is not authority.

The manager owns scheduling, retry state and workers, not a second peer store or
network runtime. Manual refresh triggers this same manager. Stream cancellation,
deadlines, notifications, scoped resources and joined lifecycle use existing
Forge components.

Automatically managed circuit addresses are separate from configured and observed
addresses. They are valid only while the corresponding reservation and direct
relay connection remain live. Changes participate in canonical signed peer records
and Identify Push. Private PSK profiles continue to reject relay use.

An explicit handler has priority over the built-in fallback. Removing it restores
the enabled built-in handler. This is Forge's composition policy, not a wire
extension or a claim that donor unregistration retains a built-in handler.
Supported protocol IDs remain unique. Private-network and direct-only relay
checks precede handler selection; custom routing cannot bypass them.

Wire interoperability does not require identical Go and Rust operational defaults.
Differences in local resource policy must be documented and bounded; codec or
manual RESERVE tests alone do not prove AutoRelay ownership or renewal.

## Acceptance Gate

Delivery requires independent exact-head review and canonical runtime promotion.
The promotion receipt binds the clean Git tree, compiled fixture and native
Go/Rust binaries to the executed cases; source inventory or standalone artifact
consistency is not a live verdict. Required proof:

- Raw node startup acquires relay access without manual reservation or refresh.
- Renewal follows the accepted expiry; failure/disconnect replaces the relay and
  withdraws obsolete addresses without changing configured addresses.
- Cancellation, public reachability and shutdown prevent late reservation commits
  and wake all callers; workers drain before peer persistence closes.
- Independent Go/Rust peers negotiate HOP/STOP and transfer data through real
  three-peer circuits, including Forge client and opt-in Forge service roles.
- Negative evidence checks reject manual-reservation substitutes, direct-path
  bypasses, mismatched identities, incomplete receipts and forced cleanup.

Run the focused 12-case canonical AutoRelay promotion gate with
`cmake --build <build-dir> --target test_forge_p2p_autorelay_acceptance -j 4`.
It uses the existing promotion wrapper, exact clean HEAD, canonical manifest and
fixture/inventory dependencies, with artifacts under the promotion directory's
`autorelay` subdirectory. This is not a full Stage 6 verdict; the global Stage 6
gate remains blocked by future-protocol and private-profile gaps.

The full registered Linux caller must run under `unshare --net --mount` with
loopback brought up inside those namespaces. AutoNAT/mDNS environment failures
from a caller without this setup are not protocol verdicts. Canonical proof
artifacts must reside on a native Linux filesystem, not a host bind-mounted
filesystem; namespace setup alone does not establish acceptance.

Unit, package and source gates complement this live proof. None independently
establish production readiness or promotion of untested profile combinations.
The focused `test_forge_p2p_autorelay` target covers manager, raw-node and session
ownership regressions without replacing the existing complete P2P suite. Native
HOP/STOP tests must observe handler entry and complete request decoding before
asserting cancellation, invalid grants, limits or coalesced byte-stream delivery.

## Fixture Lifecycle And Native Address Ordering

The registered mDNS participants remain alive until both provisional echo
receipts exist. Only immutable post-exit receipts are accepted; operation,
shutdown and forced-cleanup failures remain fatal. The Forge fixture preserves
the primary exception when shutdown also fails, recording shutdown diagnostics
separately after workers have joined.

The legacy QUIC DCUtR fixture waits for Identify from the same authenticated
relay connection and a matching native `NewExternalAddrCandidate` before creating
the circuit handler. Rust's `swarm/src/lib.rs` suppresses candidate delivery for
already confirmed external addresses, while `protocols/dcutr/src/behaviour.rs`
captures candidates when that handler is constructed. Address confirmation must
therefore follow native candidate delivery. A donor-Swarm regression exercises
both orders; stale Identify from a replaced connection cannot open the barrier.
Bounded native DCUtR events survive the final receipt. This fixture correction
does not implement PR10 path automation or change the libp2p wire.
