# Configured Plugin Evidence

The `plugin-node` suite uses the official node plugin, Secrets and the selected
ObjectDB backend. Its initial transport/DNS/PSK cases are not whole Stage7
acceptance and do not replace the raw-node suites.

## Opt-In Go Echo Completion Barrier

Only the configured Go-to-Forge public QUIC and TCP/TLS `echo_large` dial lanes pass
`--echo-completion-barrier stop-file`. This changes our test CLI synchronization,
not go-libp2p, its vendored dependencies, protocol IDs, payload or wire encoding.
All CLI invocations without this flag retain immediate-disconnect behavior.

1. Go completes the actual 192 KiB echo and automatic Identify on the same
   connection. The opt-in lane retains that actual stream as well as the host:
   native Go QUIC `stream.Close()` calls `CancelRead`, so closing the stream
   immediately could still race Forge's FIN even with an open host. Go atomically
   publishes an `exchange_observed`, `final: false`
   witness through its `--ready-file`. No terminal result exists yet.
2. The existing runner waits for Forge's successful response write and
   `async_close`, then checks the Go witness against that actual authenticated
   session: identities, numeric sockets, application connection/stream and
   same-connection Identify. A failed or canceled Forge close does not release
   the success barrier.
3. The runner requests the Go StopFile first, leaving the Forge host alive
   while Go closes its retained stream. Go waits only within its existing
   20-second dial context, checks stream close and `h.Close()`, then atomically
   publishes its terminal `--result-file`. Only after the Go process joins
   does the runner request and join Forge shutdown. Simultaneous host stops
   could otherwise race the Go stream close with Yamux GOAWAY. Missing stop,
   cancellation or failed close is an error, never graceful success.
4. Acceptance requires both process joins, Forge's final joined receipt and a
   distinct Go terminal receipt with the same exchange facts as its independently
   captured interim witness. An interim witness cannot substitute for terminal
   evidence.

The original immediate-exit RED artifacts (`development-quic-repeat`,
the TCP/TLS failure in `development-matrix-mdbx` and earlier smoke artifacts)
remain separate. A synchronized PASS does not relabel
those outcomes as successful graceful completion. There are no arbitrary
success delays, ignored cancellations or additional runtime owners.

## Opt-In Go Listener Orderly FIN

Configured Forge-to-Go public QUIC echo lanes additionally pass
`--echo-listener-completion orderly-fin` and a distinct `--echo-witness-file`.
The listener writes the actual framed response, waits under a 20-second stream
deadline for real request EOF, and only then checks `stream.Close()`. Extra
request bytes, reset, timeout and connection loss are failures, not EOF.
This avoids the fixture's immediate full close sending `STOP_SENDING` before
Forge has sent its request FIN. Donor libraries and default echo behavior are
unchanged.

The separate interim witness contains actual authenticated stream/connection
identities, sockets, payload hash, observed EOF and checked stream close.
The runner validates it against Forge's completed write/close before stopping
either host. Go's final receipt follows checked host close and handler join.
Acceptance checks both receipts against their separately captured files;
the interim witness cannot claim host shutdown. A failed handler or missing
completion remains a nonzero process failure.

## Opt-In Rust Dial Completion

Only the configured Rust-to-Forge public QUIC `dial/echo_large` lane uses the
same StopFile barrier flag. The actual stream stays in its original exchange
future while the existing Swarm loop continues to run. Its 15-second deadline
includes read/write, waiting for the stop predicate and checked stream close.
The ready-file witness contains the actual payload hashes and the authenticated
`ConnectionEstablished` identity and remote address. Rust's outbound public
event does not expose a local socket, so the receipt leaves it null rather than
copying the dial configuration or Forge's observation.

The runner first verifies Forge's response write and stream close, then allows
Rust to close that same stream. After the command drops the Swarm, the existing
task owner joins its captured tasks; only then is the separate terminal receipt
written and checked before Forge shutdown. Missing stop, early connection loss,
changed connection, failed close or failed join is an error. Default CLI,
private and TCP behavior are unchanged.

This proves the actual exchange, checked application-stream close and completion
of fixture-owned tasks. It does not promote the independent PR12 upstream Rust
QUIC whole-host shutdown gate or remove its documented `NOT_PROVEN` limitation.
No donor-library observation patch is applied by this barrier.

## Independent DHT Value Operations

The separate `plugin-dht` suite has 16 closed cases: `/pk` and `/ipns`, Go and
Rust, official Forge client and server roles, and MDBX/RocksDB. Each case owns
three processes with distinct identities and fresh stores. Writer PUT finishes
and joins before a new Reader starts; the Reader receives only the still-live
Server's address. No local PUT/GET shortcut or writer cache is accepted.

Forge clients use `node::dht_api` and require real remote response/acceptance
counts, quorum and byte-exact selected records. Donor readers compare their
actual GET result with the existing golden fixture. The fixture does not invent
donor record hashes, and the listener diagnostics are not a durable-reopen proof.
Native authenticated peer/socket observations bind the initial connection;
they are not mislabeled as captured per-RPC negotiation events. Successful
cross-implementation value operations supply causal Amino wire evidence.

Final source validation runs after the shared process owner joins all actors.
It binds the exact commands, separate store paths, terminal/readiness files and
their post-join captures. The canonical acceptance checker uses the same pinned
donor, binary, worktree and immutable-head checks as `plugin-node`, with distinct
artifact names. Neither configured subset promotes the whole Stage 7 result.

## Configured AutoNAT Boundary

The `plugin-autonat` selection has eight cases: MDBX/RocksDB, official
Forge client/service roles, v1 with Rust and v2 with Go, over TCP/Noise/Yamux.
It reuses the existing isolated AutoNAT network and process owners. Other donor,
version and transport combinations remain native-suite evidence, not implicit
configured-plugin coverage. All eight development exchanges passed; final
immutable-head acceptance is tracked separately from that uncommitted run.

Only accepted YAML and the ordinary authenticated connect operation start the
Forge client. Manual probe API calls are forbidden. Both implementations must
independently record the actual authenticated control and fresh dialback
connections; the donor must also report the correlated successful RPC. A
single v1 observer does not satisfy the default host quorum: that host remains
`unknown`, without a fabricated public vote. V2 evidence additionally binds its
verified address-level state to the exact requested listener.

The explicit configured validator boundary requires joined official shell and
scheduler owners, plus typed rejection by a retained diagnostics API after
detach. It must not synthesize the unavailable raw-node post-stop snapshot.
The default raw validator keeps its existing post-stop and manual-probe checks.
Receipts cannot choose or downgrade their own validation boundary.

These cases require isolated Linux networking with public-classified numeric
addresses and no external route. Loopback/LAN advertisement on macOS is not a
successful AutoNAT exchange and cannot substitute for this test. No donor
library patch, permissive address filter or single-observer quorum override is
introduced for configured acceptance.

## Configured PubSub Policy

The separate `plugin-pubsub` suite has eight cases: Go/Rust, MDBX/RocksDB and
YAML-only versus an authoritative typed pre-start snapshot. The official plugin
owns Secrets, ObjectDB and network lifecycle. Both peers exchange actual signed
messages over TCP/Noise/Yamux. The YAML case selects v1.1 with a 128-byte payload
limit; the typed case selects v1.2 with a 96-byte limit despite a competing
v1.0 YAML setting. Exact-limit binary payloads arrive at the donor; the next byte
is rejected by the native engine before publication.

The typed snapshot retains the application tracer and score callbacks. This
proves configured callback preservation and invocation, not the full native
scoring or extension matrix. IDONTWANT and Partial Messages are not claimed by
these cases. Existing raw-node extension evidence remains separate.

Forge captures its authenticated session through public diagnostics; the donor
captures its actual connection. Their identities and numeric sockets must agree.
Identify failure is recorded, not converted to success: the GossipSub-only Rust
fixture does not advertise Identify, which is not required to negotiate and
exchange GossipSub messages on an authenticated connection. The base transport
and DHT fixture's stronger Identify checks are unchanged.

Both actors acknowledge bounded prepare controls before host stop. The shared
process owner joins donor and Forge; Forge joins its application shell and
scheduler. Only then can the canonical checker bind readiness, final receipts,
controls and independent prepare captures to the exact commands and binaries.
The development rerun passed all eight cases after fixing fixture bookkeeping
and redaction. Earlier failed artifacts are retained. This is not an immutable
exact-head acceptance result or whole Stage 7 promotion.

## Configured AutoRelay Ownership

The `plugin-relay` selection composes the existing eight public TCP/Noise
AutoRelay client/service cases through MDBX and RocksDB. The original orchestration
and process owner remain shared. The only Forge launch extension selects the
real ObjectDB backend. The plugin loads identity through Secrets; initial seed
connections enter its ordinary API, never a manual reservation or refresh call.

Client evidence retains native lease acquisition/renewal, traffic after initial
expiry, relay loss/replacement and withdrawal. Independent Go/Rust Identify and
Push observations supply the circuit address used by the actual echo dialer.
Service evidence retains donor reservation and byte-transfer checks. No QUIC,
DCUtR or private-profile result is inferred from this TCP selection.

Configured lifecycle evidence intentionally differs from a raw-node snapshot:
the baseline is `before_seed` after successful official startup, not a fabricated
`before_start`. After awaited plugin and scheduler joins, a new call through
the retained diagnostics API must throw typed `plugin_not_initialized`; the
retained host-event subscription must be inactive and return no next event.
Native observations stop at `pre_stop`. The configured validator is selected
by its caller and does not replace the original raw validator's stricter
post-stop counter observations. Neither path substitutes zero-filled rows for
unavailable diagnostics.

The first development matrix passed all eight cases with semantic and post-join
source validation. This remains separate from final immutable-head acceptance.

The configured validator also requires both actual Forge seed connections and
all three completed incoming lifecycle echoes. Each echo is tied to its donor
source identity and previously observed relay carrier; request/response bytes,
hashes, completed writes and stream closes cannot be replaced by donor success
or by a configured endpoint. Missing or inconsistent Forge receipts reject the
case, including when file capture indexes are recomputed.

## Configured Local Discovery And Events

The focused ObjectDB suite separately checks independent provider discovery,
provider quotas/reopen, and automatic Rendezvous discovery through official
plugin APIs. These checks do not claim new donor interoperability or crash
recovery.

Two opt-in CTest entries exercise the actual host LAN interface through both
available disk backends: `test_forge_p2p_plugin_host_events_live` and
`test_forge_p2p_plugin_mdns_live`. Set `FORGE_TEST_P2P_LAN_ADDRESS` to an owned
non-loopback IPv4 address. Missing input produces an explicit `NOT_RUN` message
and CTest skip. Invalid input, absent listener binding or failed multicast is
a failure, not a skip. These cases are disabled in the ordinary Boost test
selection and explicitly selected by their separate CTest commands.

The event case uses real authenticated inbound Identify, a configured single
observer threshold, unread confirmation/expiry events, latest-state resync and
joined shutdown. It does not claim default-quorum reachability or public NAT
success. The mDNS case uses a separate test PSK namespace, discovers without
peer coordinates, authenticates Noise/Yamux, exchanges Ping and observes
retirement after the advertiser stops. It is local configured Forge parity;
the native Go/Rust multicast evidence remains a separate suite.
