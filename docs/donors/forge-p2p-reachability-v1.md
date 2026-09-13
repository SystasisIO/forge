# P2P Reachability: Stage 6 PR6

## Scope And Baseline

This iteration owns host-local observed addresses, AutoNAT v1/v2 clients and
opt-in services, periodic Ping and typed host-state subscriptions. It does not
implement AutoRelay, DCUtR orchestration, mDNS, UPnP or plugin configuration.
The implementation roadmap remains
[Stage 6](../iterations/forge-p2p-production-implementation-v1.md#stage-6-donor-parity-reachability-and-path-management).

Pinned references inspected:

- Go libp2p `9cfe2cc00be5b20a0be737f002c99f81b92255c5`:
  `p2p/host/observedaddrs/manager.go`, `p2p/host/autonat/{autonat,client,svc,dialpolicy,options}.go`,
  `p2p/protocol/autonatv2/{client,server}.go` and the two protobuf definitions.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/autonat/src/v1/` and `protocols/autonat/src/v2/` client/server handlers.
- libp2p specifications `6b6203ee6f62938ce67efdb33498173f475851c0`:
  `autonat/autonat-v1.md` and `autonat/autonat-v2.md`.

## Accepted Mechanics

Identify observations belong to authenticated connections, not a peer-address
cache entry. An observation must match a real local listening transport;
an outbound ephemeral local port cannot be substituted with an unrelated
listener. Go counts an IPv4 address or IPv6 /56 as one independent observer,
requires four observers and ranks at most three external addresses per local
transport. Forge reuses that grouping and deterministic selection, with explicit
bounded storage and expiry. Disconnect removes the connection's contribution.

Native QUIC sessions may report their shared socket's wildcard bind rather than
a concrete packet destination. Forge retains that actual bind as the local key
only when the exact wildcard listener, address family and port are owned by the
node. It does not infer a concrete local IP from Identify. Remote and reported
addresses still must be concrete; an unspecified TCP local endpoint is rejected.

An observed-address quorum is not an Internet reachability verdict. In particular,
LAN address agreement does not imply Internet Public.

AutoNAT v1 is node-level evidence. Forge's explicit policy requires three
independent, nonconflicting observer groups for that verdict rather than copying
Go's repeated single-observer confidence counter. Evidence expires and belongs
to the current local address generation; an address change invalidates previous
evidence and rejects late results from old probes. Refusal, overload, cancellation
and malformed protocol responses are not negative votes.

The node reachability policy defaults to a 20-second exchange budget. Pinned Go
`p2p/host/autonat/options.go` sets its service dial timeout to 15 seconds, and
`svc.go::doDial` deliberately waits for that deadline before returning a negative
response, even after an immediate dial failure. Forge's former 10-second default
could therefore discard valid Go negatives. Explicit timeout overrides remain
supported; the 20-second default is Forge policy, not a copy of Go's 30-second
client request timeout. The explicit-clock regression models completion at
15 seconds; actual Go negative interoperability requires separate live evidence.

Forge dial-back cleanup preserves local gater/resource/internal failures rather
than reporting them as remote dial failures. V1 maps local policy/resource refusal
to `E_DIAL_REFUSED`; v2 uses `E_DIAL_REFUSED` for policy and `E_REQUEST_REJECTED`
for resource admission. Other typed local failures produce `E_INTERNAL_ERROR`.
Cancellation and terminal closure do not become NAT negatives.

AutoNAT v2 verifies one selected address. A positive requires the nonce of the
current operation and a matching actual incoming transport. The dialer may have
a different authenticated Peer ID from the service: Go intentionally uses an
independent dialer host. A server `OK` without a corresponding dialback proves
nothing. `E_DIAL_BACK_ERROR` with a valid observed dialback still proves the
address. LAN positives remain address-local; only a public-address positive may
raise effective Internet reachability. Several negative v2 address results do
not independently classify the entire node as private.

The v2 service's `DialStatus_OK` means that the isolated authenticated connection
was established and the nonce write completed, not that `DialBackResponse` was
decoded. Pinned Go `9cfe2cc` `p2p/protocol/autonatv2/server.go::dialBack` ignores
the result of its final read, bounded to five seconds; the client handler closes
and then resets its dial-back stream. Forge's private `autonat_v2_dialback` helper
therefore treats ACK as an optional delivery wait. Its write/read/normal-close
phase is bounded to five seconds and capped by the parent deadline. Native QUIC
or Yamux reset/closure after a successful nonce write does not invalidate that
service result. The same failure before the write completes is
`E_DIAL_BACK_ERROR`, never success.

Parent cancellation/deadline and local resource/runtime errors remain failures.
The helper cancels only its child stream on the optional-wait deadline, awaits
terminal stream cleanup, and retires owner-accessing deadline callbacks. The node
then awaits isolated native-attempt cleanup before writing the parent response.
This does not relax the client's independent nonce and actual-transport proof,
change the shared exception mapper, or publish the isolated attempt as a normal
peer session. Unit regressions live in `p2p_autonat_exchange/v2_dialback_*`; the
optional-ACK timeout case uses the actual five-second production bound with an
eight-second external test wait.

## Wire Corrections

- V1 `Message.dial` wraps `Dial.peer`; it is not a direct `PeerInfo` payload.
- Omitted proto2/proto3 scalar defaults must decode correctly, including zero
  status/index and an empty successful v2 dial-back acknowledgement.
- V1 responses encode the proto2 status explicitly, including `OK = 0`.
  Rust `v1/protocol.rs::DialResponse::from_proto` requires `Some(OK)` together
  with an address; Go's default-valued getter alone does not prove that presence
  contract. The golden response follows Rust `DialResponse::into_proto`.
  V2 remains proto3: its empty successful acknowledgement is the one-byte
  length-delimited frame `00`, not an absent response or EOF.
- Client responses are length-delimited across arbitrary transport reads.
- V2 selects one address and permits at most one dial-data request for that
  selected index. The total requested data is bounded; it is not an allocation
  or send loop controlled by an unbounded remote integer.
- A v1 service validates the requested identity against the authenticated caller
  and applies the observed-IP dialback restriction. Service probes must not
  publish normal sessions or update topology, peer history or dial backoff.

The pinned Rust v2 server selects with `pop()` but responds with index zero in
its current handler. Bilateral interoperability must therefore include a single
candidate. This is a documented donor limitation, not permission to reinterpret
multi-address responses or skip the Rust direction.

An AutoNAT exchange pins one identified session that advertises its requested
protocol. A newer same-peer dialback connection must not displace that control
connection merely because it is newer: Rust's v2 service dialback connection
does not provide the request handler. Retirement of the selected session fails
the exchange without silently switching connections. Background probes reuse
qualified sessions and do not introduce another redial loop.

For Forge-client/Rust-server v1, paired evidence may contain the client's real
inbound `secured` event before its `upgraded` event. Rust
`v1/behaviour/as_server.rs` answers after a fresh authenticated outbound dial to
the requested address; it does not await the other side's upgrade callback.
The validator still requires that client-side authentication, the separate Rust
connection ID, exact peer/address, correlated typed Request/Response and Forge's
native positive vote. It never relabels `secured` as `upgraded`, fabricates the
Rust local socket, or applies this alternative to v2.

## Ownership And Events

Observed-address and reachability state are separate bounded private components.
Neither owns a clock, scheduler or persistence. Node lifecycle owns runtime work;
the existing Asio notification and P2P cancellation/resource components are reused.

Host subscriptions receive an atomic initial snapshot and monotonic generation.
A slow subscription retains one latest complete state, not an unbounded history.
Replacing an unread state sets `resync_required`; the returned complete snapshot
is the new baseline. Closing a subscription or its source wakes pending readers.
The default subscription bound is 64. Events are observations, not a control path
for topology, admission or persistence.

The synchronous getter returns that same complete published snapshot. Manager
state changes, including TTL expiry, become visible with a new event generation
when published; the getter does not attach an old generation to newer state.

Awaited shutdown closes admission and joins active and already-retiring sessions
before stopping their listeners. In particular, a shared QUIC UDP listener must
remain available while session owners send connection-close frames. Shutdown
preserves the first cleanup error but still joins the remaining owners. Closing
the socket first can leave a remote cached connection alive until its idle
timeout; resetting healthy connections on an unrelated stream timeout is not a
substitute for correct shutdown ordering. The emergency synchronous stop path
remains separate. Concurrent-stop and preserved-error regressions use actual
QUIC sessions; the managed resolver failover regression retains its existing
timeouts and retry bounds.

Native stream termination and reset-drain completion are separate QUIC events.
A per-stream recovery claim precedes native shutdown and keeps terminal close
waiting until the reset owner exits, including write-side cancellation and
failure recovery. The regression drives a real native close callback while that
owner is held and checks both successful close and preserved primary error.
It does not substitute a synthetic callback or wait on unrelated connection jobs.

The automatic DCUtR fallback must not begin another five-second wait after a
shutdown-canceled dial. It observes the existing lifecycle notification and
session admission instead of polling. Periodic heartbeat and relay-discovery
workers retain their absolute deadlines across admission notifications, so
waking the fallback does not accelerate unrelated network work. Regressions
cover stop-before-wait, stop-during-wait, admission, periodic timing and shutdown.
QUIC reset metrics count each stream once even when a local reset is followed
by a remote RESET_STREAM; this does not suppress either native transition.

Private profiles retain TCP/Yamux PSK protection. AutoNAT Internet egress requires
explicit `allow_internet`; the default is `deny_external`. Both successful enabled
exchanges and denial-before-I/O controls are required. A denial-only fixture must
not replace the approved positive interoperability requirements.

## Evidence Status

The old loopback `autonatv2` stub is removed. Opening the protocol or returning
an unasserted reachability enum was not a positive exchange proof. The mandatory
41 actual exchanges and controls replace that stub in both focused and full runs.

The pinned Rust Identify behaviour uses the legacy peer-record decoder in
`protocols/identify/src/protocol.rs`. Standard `libp2p-peer-record` envelopes
with payload type `03 01` are supported separately by
`PeerRecord::from_signed_envelope_interop` in `core/src/peer_record.rs`.
The fixture preserves the behaviour's original result and performs a separately
labelled authenticated Identify exchange with a 4096-byte bound. It retains raw
bytes and hashes before validation, uses the donor envelope verifier, and checks
the signer, Identify key, record identity and authenticated remote identity.
This is not a claim that the pinned behaviour consumed the original envelope.

The pinned Rust relay server sends a successful reservation with `voucher: None`
in `protocols/relay/src/protocol/inbound_hop.rs`. Forge-to-Rust evidence therefore
correlates the actual client reservation with the server's fresh
`ReservationReqAccepted` event, returned addresses and expiry. Both process
snapshots must be indexed and belong to successful terminal processes. An empty
voucher alone is insufficient; reservation evidence is not circuit-echo evidence.

The Go TCP fixture observes the pinned donor's public `transport.Upgrader`,
`sec.SecureTransport` and `network.Multiplexer` delegates. Its bounded trace
retains actual multistream frames, including rejected proposals before fallback,
and successful security/muxer transitions. Early muxer negotiation is a distinct
security-handshake observation, not an invented post-security multistream exchange.
No donor source or Forge production API is changed for this instrumentation.

An explicit outbound fixture stream carries a one-shot context binding through
the donor's `swarm.Conn.NewStream` into `MuxedConn.OpenStream`. This associates
application completion with the exact observed stream; connection/protocol labels
alone are insufficient. Built-in inbound Identify remains the donor handler:
the wrapper observes bounded response framing, byte counts and hashes, then
successful delegated close. This proves response writing, not remote delivery.
Application bodies and security handshake bytes are not retained. Trace limits
never alter forwarded I/O; overflow instead invalidates the evidence.

Listener traces are captured after joined host shutdown. Failed background
upgrades remain in the report but cannot replace the selected successful target,
and a failed target cannot borrow a different connection's success. Controlled
parser/binding regressions and Go race tests complement the bilateral native
TCP Noise/TLS Identify and echo runs. These are capture-layer checks: Rust
upgrade observation and strict same-exchange integration into the acceptance
checker are still required before the ordered-upgrade proof gate can pass.

Positive exchanges use isolated Linux network namespaces with public-classified
numeric addresses, no external interface and no default route. This exercises
unchanged production address filters without sending probes into the Internet.
The client and server have distinct identities and a real fresh dialback must
occur. A second address on the same client supplies the v2 dial-data payment
case. Namespace setup evidence is separate from protocol outcome evidence;
fixture processes must finish before their namespaces are removed.

This setup proves protocol interoperability in a controlled network, not router
traversal or Internet-wide production reachability. Real NAT and long-running
hostile-network proof remain later delivery gates. No donor test-only private
address allowance or fabricated successful address validation is permitted.

Implementation is in progress. New codec, observed-address, event and state tests
are not by themselves proof of autonomous lifecycle or live interoperability.
PR6 is not complete until node integration, focused/package suites, exact-head
Go/Rust exchanges and independent review pass. No Stage 6 inventory capability is
promoted on the basis of this note.
