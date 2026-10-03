# QUIC + P2P

`forge_net_quic`, `forge_net_transport` and `forge_net_p2p` form the FORGE peer data-plane
foundation. QUIC is one concrete transport. `forge_net_transport` is the reusable
stream/session substrate. P2P is the peer/session/protocol-stream layer above
transport-shaped connections.

Local guides:

- [QUIC README](../../libraries/net/quic/README.md)
- [P2P README](../../libraries/net/p2p/README.md)

## Задача

Long-lived peer streams need different semantics than HTTP control APIs:
transport identity, handshake limits, protocol negotiation, direct/relay path
selection, reachability, stream backpressure and deterministic shutdown. Those
concerns need reusable FORGE primitives without embedding any product protocol.

## Layering

```text
forge_asio::runtime
  -> forge_multiformats
      -> multiaddr
  -> forge_net_transport
      -> endpoint/stream/session/frame contracts
  -> forge_net_tcp / forge_net_stcp / forge_net_yamux / forge_net_quic
      -> concrete transport implementations and adapters
  -> forge_net_p2p
      -> peer identity
      -> security upgrade, mux selection and protocol negotiation
      -> protocol streams
      -> peer store/path manager/relay
```

Concrete network libraries know sockets, TLS, QUIC engines and muxing mechanics.
`forge_net_transport` exposes the common byte-stream/session contracts. P2P knows
peers, libp2p security semantics and protocol streams. Application protocols
live above P2P and define their own messages, durability and authorization.

## QUIC Responsibilities

- UDP socket/timer integration with Asio.
- ngtcp2 QUIC engine and OpenSSL 3.0+ TLS backend.
- ALPN, certificate verification, pinned fingerprints and mTLS checks.
- Framed stream codec and transport limits.
- Backpressure for streams, queued bytes and inbound datagram queues.

QUIC does not own peer discovery, relay policy or application protocol naming.

## P2P Responsibilities

- Local peer identity from transport certificate material.
- Direct connect with expected peer checks.
- Protocol handler registry and stream opening by `protocol_id`.
- Peer exchange and peer store freshness.
- Explicit relay reservation and cancellation.
- Reachability probing and hole-punch attempt orchestration.
- Path scoring/backoff across direct and relay candidates.

P2P does not promise exactly-once delivery, durable storage or product
authorization. DHT/rendezvous discovery belongs in `forge_net_p2p`; product plugins
must not replace it with parallel discovery loops.

For application/plugin composition, `forge::plugins::net::p2p::node` is the production
host facade above `forge_net_p2p`. It applies config, starts the node and mounts
protocol/API contributions. G.2 narrows the API around typed remote access and
local network information. Durable queues, application fan-out and read-only
diagnostics move to focused plugins or product layers. Product plugins should
not create a second node or run ad hoc retry loops against raw `forge::net::p2p::node`.

## Production P2P Direction

`forge_net_p2p` targets a clean C++23 implementation of a libp2p-compatible network
stack. Compatibility means protocol compatibility (`протокольная
совместимость`): when FORGE declares support for a libp2p protocol, an FORGE node
must be able to talk to go-libp2p and rust-libp2p nodes using the same wire
formats, handshake, Peer ID, Identify, Ping, QUIC profile and later supported
protocol rules.

This does not mean copying Go/Rust runtime architecture or leaking their public
API shape into FORGE. FORGE public APIs should keep C++/Boost-style network terms
such as `endpoint`, `resolver`, `listener`, `connector`, `session`, `stream`
and `protocol_id`. `multiaddr` is still a first-class FORGE multiformats concept
because it is the libp2p address contract; `p2p::endpoint` is a typed P2P view
over a multiaddr, not a parallel source of truth.

Production network mechanics belong in `forge_net_p2p`, not in plugin-local
workarounds: identity, keys, endpoint/address encoding, protocol negotiation,
Identify, Ping, peer/path store, relay, AutoNAT, DHT and pubsub. The
`forge::plugins::net::p2p::node` plugin only maps config into the node, owns application
lifecycle, mounts route/API contributions and exposes safe application APIs.
Product extensions must not build parallel network-discovery, relay or gossip loops.

Ed25519, Secp256k1, ECDSA and RSA are all mandatory compatibility key families.
RSA is required for IPFS/mainline DHT compatibility. Secp256k1 and ECDSA are
required for blockchain-like networks built on top of FORGE and plugins.

Multiformats uses the byte-native `forge.codec.base58` API directly. Encoding
does not belong to the crypto or network layers, and those layers must not add
parallel Base58 implementations or scattered character/byte casts.

## Implementation Blocks

This section is the canonical roadmap for the network/P2P blocks. Library
READMEs may link here, but must not define a second block order.

### Block A: Roadmap Rebase + Checkpoint

- The work order changes: transport substrate work goes before continuing P2P
  protocol expansion.
- The previous conclusion "Yamux is private to P2P" is superseded. Yamux is a
  reusable muxer because it is needed by libp2p TCP, STCP/API stacks and future
  stream-session transports.
- Current order: `multiaddr -> forge_net_transport -> tcp/stcp/yamux/quic -> p2p
  rebase -> p2p completion -> forge.api.transport`.
- Existing P2P achievements remain valid checkpoints: QUIC, Ping, Identify,
  Relay v2, AutoNAT/DCUtR, DHT/Rendezvous component layer and GossipSub v1
  proof are not discarded. They must be preserved while the substrate is
  cleaned up.

### Block B: First-Class Multiaddr

- `forge_multiformats` owns a first-class `multiaddr` concept: `component`,
  `protocol_code`, `encapsulate`, `decapsulate`, text roundtrip and binary
  roundtrip.
- Supported address components for this block: `ip4`, `ip6`, `dns`, `dns4`,
  `dns6`, `tcp`, `udp/quic-v1`, `ws`, `wss`, `p2p` and `p2p-circuit`.
- `/ws` and `/wss` are parse/store only. Dial/listen rejects them with a typed
  P2P error until a real browser/proxy requirement exists.
- `p2p::endpoint` becomes a typed view over `multiaddr`, not the source of
  truth for address encoding.

### Block C: `forge_net_transport` Foundation

- `forge_net_transport` owns only low-level reusable contracts: `stream`, `session`,
  `frame`, `limits` and `exceptions`.
- Add Asio-style `stream_connector`, `stream_listener`, `session_connector`,
  `session_listener` and transport registry primitives.
- `forge_net_transport` must not import or model `forge_api_core`, `forge_net_p2p`, concrete
  QUIC/TCP types, TLS policy, Yamux policy, Peer ID, Relay, DHT, Rendezvous or
  GossipSub.
- Builders are allowed only as composition helpers over real owner-shaped
  connector/listener/session implementations. They must not hide the
  implementation or expose decorative options.

### Block D: Reusable Network Layers

- D.1 `forge_net_tcp`: Boost.Asio TCP adapted to `transport::stream`.
- D.2 TCP upgrade surface: `tcp::connection` owns the native socket until an
  upper layer either turns it into `transport::stream` or releases it for a
  TLS/security upgrade.
- D.3 `forge_net_stcp`: TCP+TLS mechanics adapted to secure `transport::stream`.
  This layer owns certificates, trust stores, fingerprint checks, ALPN and TLS
  handshakes, but not P2P Peer ID verification, libp2p protocol choice or
  multistream decisions.
- D.4 `forge_net_yamux`: reusable muxer from `transport::stream` to
  `transport::session`, donor-derived from go-libp2p and rust-libp2p Yamux.
- D.5 `forge_net_quic`: QUIC adapted to native `transport::session`.
- Current checkpoint: `forge_net_quic` exposes `quic::as_transport_stream(...)`,
  `quic::as_transport_session(...)`, native
  `transport::session_connector/session_listener` construction and
  `transport::registry` registration for `/quic-v1` endpoints.
- WebSocket transport is not implemented in this block. Product
  `forge_net_websocket` remains an application WebSocket API, not a libp2p transport
  claim.

### Block E: P2P Rebase

- `forge_net_p2p` uses first-class multiaddr, the transport registry and reusable
  network layers.
- QUIC path: `/udp/.../quic-v1 -> forge_net_quic -> transport::session`.
- TCP path: `/tcp/... -> forge_net_tcp -> libp2p security upgrade -> forge_net_yamux ->
  transport::session`.
- E.1 checkpoint: QUIC is hidden behind a private P2P direct profile. The
  profile may inspect native QUIC certificates before erasing the connection to
  `transport::session`, because Peer ID verification is P2P semantics.
- E.1 checkpoint: relay and DCUtR use reusable `forge_net_yamux`; the old private
  P2P Yamux runtime is removed.
- E.1 checkpoint: `/tcp`, `/ws` and `/wss` endpoints became parseable. E.2a
  supersedes the `/tcp` part by wiring direct TCP; `/ws` and `/wss` still return
  typed unsupported from P2P dial/listen.
- E.2a checkpoint: direct `/tcp/...` now uses the libp2p TCP stack shape:
  `forge_net_tcp -> multistream-select -> Noise -> forge_net_yamux -> transport::session`.
  FORGE, go-libp2p and rust-libp2p live scenarios cover Ping, Identify and a
  framed echo stream in both FORGE directions.
- E.2b checkpoint: direct TCP now prefers the libp2p TLS security branch:
  `forge_net_tcp -> multistream-select -> /tls/1.0.0 -> forge_net_stcp -> forge_net_yamux ->
  transport::session`. Noise remains a supported fallback. FORGE, go-libp2p and
  rust-libp2p live scenarios cover Ping, Identify and a framed echo stream for
  both TCP security branches.
- E.2b checkpoint: `/ws` and `/wss` remain parse/store only at multiaddr level.
- E.2c is the mandatory cleanup gate before continuing P2P feature work:
  tighten reusable Yamux resource semantics against libp2p donor behavior. This
  block must reject invalid window/buffer option combinations up front, because
  an implementation must not advertise an initial receive window it cannot
  buffer under honest peer traffic.
- E.2c must make stream-local resource abuse reset only the offending Yamux
  stream where the donor behavior permits it. A stream buffer overflow must not
  automatically fail the whole mux session; existing healthy streams must keep
  working after the reset. True malformed frames, protocol violations and
  session-level terminal errors still close the session with typed Yamux
  exceptions.
- E.2c should replace the hot `read_frame` erase-and-shift path with an
  offset/ring-buffer style parser before bulk data paths depend on Yamux. This
  is a performance hardening item, not a wire-format change.
- E.2c should release terminal Yamux stream resources as early as safety allows.
  Reset streams must release buffered bytes and wake waiters immediately, while
  stream map entries may remain until a safe reclamation boundary so existing
  handles can still report typed `stream_reset` instead of generic closed.
- E.2c keeps Yamux internal timeout-free by design: deadlines are caller policy
  and must be applied by P2P/API/transport users around operations. The cleanup
  must verify that this remains an explicit contract, not an accidental missing
  feature.
- E.2c does not add P2P protocols, API bindings or `/ws`/`/wss`
  dial/listen paths.
  It is a cleanup block for reusable `forge_net_yamux` plus donor-derived regression
  tests, and it must remain compatible with the already proven TCP Noise/TLS
  and QUIC/P2P scenarios.
- E.2c implementation files:
  `libraries/net/yamux/session.cpp`,
  `libraries/net/yamux/include/forge/net/yamux/options.cppm`,
  `tests/yamux/yamux_tests.cpp` and `docs/donors/forge-yamux-v1.md`.
  No `libraries/net/p2p`, API, QUIC, TCP or STCP runtime code should change unless
  a regression test proves the reusable Yamux contract cannot be fixed in
  `forge_net_yamux` alone.
- E.2c option invariants:
  `validate_options()` must reject `initial_window == 0`,
  `max_stream_window < initial_window`,
  `max_stream_buffer < initial_window`,
  `max_session_buffer < initial_window`, zero stream/backlog/buffer limits and
  wire-impossible frame sizes. The important contract is that FORGE never sends a
  `WINDOW_UPDATE|SYN` receive credit larger than the amount it can accept from
  one honest peer stream before application reads.
- E.2c stream overflow behavior:
  `handle_data(...)` must treat stream-buffer and attributable session-buffer
  overflow as a stream-local reset. It should mark only that stream reset, clear
  its buffered inbound data with correct `session_buffer_` accounting, wake
  stream waiters, send `RST` for that stream and continue the session read loop.
  Local users of the reset stream observe typed
  `forge::net::yamux::exceptions::stream_reset`; unrelated streams remain usable.
- E.2c session-failure behavior:
  malformed frame version/type/flags, stream zero misuse, invalid stream ID
  parity, duplicate stream IDs, oversized DATA frames and GOAWAY/error paths
  remain session-level failures with typed Yamux exceptions and GOAWAY where the
  current implementation already sends it.
- E.2c parser cleanup:
  `read_frame(...)` should stop erasing consumed bytes from the front of the
  receive buffer after every frame. Use an explicit consumed offset, compact
  only when useful, or an equivalent ring-buffer style parser. Preserve partial
  frame handling, multiple buffered frames and `max_frame_size` rejection.
- E.2c reclamation cleanup:
  extend the existing `is_reclaimable_stream_locked(...)` and
  `reclaim_closed_streams_locked()` path so terminal streams are removed before
  new stream cap checks, while reset handling eagerly clears inbound buffers,
  subtracts bytes from `session_buffer_` and wakes read/window waiters. This
  preserves typed reset delivery for existing handles and still releases memory
  and budget promptly.
- E.2c tests to add before implementation:
  `test_forge_yamux` must fail on invalid options where
  `max_stream_buffer < initial_window` and
  `max_session_buffer < initial_window`; it must prove a stream-buffer overflow
  resets only the offending stream while a second stream still transfers bytes;
  it must prove an attributable session-buffer overflow follows the same
  stream-local policy; it must preserve malformed-frame session-failure tests;
  it must cover the new parser with partial frames, multiple buffered frames and
  trailing buffered bytes; and it must prove reclamation releases stream count
  and buffer budget before another stream depends on those resources.
- E.2c donor/doc update:
  `docs/donors/forge-yamux-v1.md` must map the new stream-local reset behavior,
  invalid option invariants, parser buffering behavior and eager reclamation
  tests to libp2p spec/go-libp2p/rust-libp2p donor expectations. If a donor
  behavior is ambiguous, the doc must state the FORGE policy and why it is
  compatible.
- E.2c static gates:

  ```sh
  rg -n "buffer\\.erase\\(buffer\\.begin\\(\\)" libraries/net/yamux/session.cpp
  rg -n "FORGE_THROW\\(|throw std::runtime_error|exceptions::raise\\(" \
    libraries/net/yamux tests/yamux
  rg -n "import forge\\.(api|p2p|quic|tcp|stcp|multiformats|websocket)|forge::(api|p2p|quic|tcp|stcp|multiformats|websocket)" \
    libraries/net/yamux/include libraries/net/yamux --glob "*.cpp"
  ```

  All three searches should be empty after cleanup.
- E.2c validation:

  ```sh
  cmake --build build/forge-typed-exceptions-debug -j 1 \
    --target forge_net_yamux forge_net_transport forge_net_tcp forge_net_stcp forge_net_quic forge_net_p2p forge \
             test_forge_yamux test_forge_quic_p2p test_forge_libp2p_interop

  ctest --test-dir build/forge-typed-exceptions-debug --output-on-failure \
    -R "^(test_forge_yamux|test_forge_quic_p2p|test_forge_libp2p_donor_matrix)$" \
    --timeout 900

  FORGE_ENABLE_LIBP2P_INTEROP=1 \
  ctest --test-dir build/forge-typed-exceptions-debug --output-on-failure \
    -R "^test_forge_libp2p_interop$" \
    --timeout 1800

  git diff --check
  ```
- E.2d implements the transport buffer API compatibility guardrail before the
  next P2P completion work. `forge_net_transport::stream` keeps
  `async_read()`/`async_read_frame()` as stable vector-returning convenience
  APIs, and adds an additive `forge.net.transport.buffer` fast path for serious
  stream consumers.
- E.2d adds transport-owned `chunk`, `chunk_builder` and `buffer_pool`
  primitives. Chunks carry safe shared byte ownership, builders provide writable
  storage before commit and the pool reuses storage within explicit cached
  buffer/byte limits instead of hiding unbounded queues.
- E.2d adds chunk-oriented stream operations:
  `async_read_chunk()`, `async_read_frame_chunk()`, `async_write(chunk)` and
  `async_write_frame(chunk)`. Existing vector methods delegate through the new
  path where safe, so current P2P/API consumers do not need an immediate
  migration.
- E.2d adds frame fast-path helpers: `decode_frame_view(...)` returns a payload
  span plus consumed bytes without allocating a payload vector, and
  `stream::async_read_frame_chunk()` uses consumed-offset buffering with bounded
  compaction instead of front-erasing the receive buffer.
- E.2d integrates the fast path with TCP, STCP, QUIC and Yamux transport
  adapters. TCP/STCP read into pooled builders; QUIC/Yamux wrap or move already
  owned stream data into chunks without adding P2P/API semantics to
  `forge_net_transport`.
- E.2d is not the final content data-plane implementation. It does not claim
  kernel zero-copy, file-backed chunks, `sendfile`, content addressing or
  `contentd` bulk-transfer readiness. Before `forge.api.transport` or content
  bulk workloads ship, a separate benchmark/throughput block must audit
  remaining copies, allocation counts and large-chunk behavior.
- E.3 checkpoint: host-level multi-transport orchestration lives in private
  `forge_net_p2p` host/node helpers, not in a new public multi-transport library and
  not inside the direct transport layer. A production node can listen on several
  direct transports at the same time, for example `/udp/.../quic-v1` and
  `/tcp/...`, and advertises the selected address set through Identify and peer
  exchange. DHT/rendezvous publication must use the same canonical host address
  source when their larger-network hardening resumes.
- E.3 replaces the single active direct listener with per-profile listeners,
  adds `node::local_endpoints()` and keeps `local_endpoint()` as a first-endpoint
  compatibility convenience. `async_listen(...)` is intentionally multi-call for
  supported direct endpoints.
- E.3 keeps `forge::net::p2p::direct` direct-only: QUIC/TCP direct profiles, direct
  listen/connect/accept and no Identify, DHT, Relay, peer exchange, address
  advertisement or path scoring ownership. Relay/circuit paths stay above direct
  and are selected by host/node orchestration.
- E.3 path selection is transport-aware for direct candidates: QUIC and TCP
  endpoint records are filtered by support/freshness/backoff and then ordered by
  endpoint score. Backoff is per endpoint, so one bad TCP or QUIC address does
  not poison the whole peer.
- E.3 hardens address hygiene: advertised direct endpoints carry
  `/p2p/<local-peer>`, learned endpoint suffixes must match the verified remote
  peer, observed addresses remain separate from listen/advertised records, and
  duplicate canonical endpoints are collapsed before they enter protocol
  documents or peer-store records.
- E.3 also fixes peer exchange layering: FORGE peer exchange is now selected with
  `multistream-select` like the other negotiated P2P protocol streams, rather
  than writing a private codec directly onto an unnegotiated session stream.
- P2P owns Peer ID, Identify, libp2p Noise/TLS payload semantics,
  multistream-select, Relay, DCUtR, DHT, Rendezvous and GossipSub.
- P2P does not own generic TCP, STCP or Yamux runtime.

### Block F: P2P Completion

- F.1 implemented checkpoint: production AutoRelay discovery lives in private
  `forge_net_p2p` host/node orchestration. `node::async_refresh_relay_candidates()`
  and the background maintenance path use the same candidate collector over
  peer store, Identify/peer-exchange records, DHT-learned peers and
  Rendezvous-learned peers. The node maintains fresh outbound relay
  reservations, prunes expired records, backs off failed relay candidates and
  uses bounded refresh before relay fallback returns `relay_not_available`.
  Manual `relay_peer` remains an explicit override. Relay discovery is not owned
  by `direct`, plugins or product loops.
- F.2 implemented checkpoint: DHT/Rendezvous discovery lifecycle is hardened in
  private `forge_net_p2p` algorithms, not transport/direct/plugins. `dht_query`
  performs bounded iterative Kademlia-style lookup with queried/failed sets,
  XOR-distance closer-peer merging, typed timeout/cancel behavior and stale
  routing/provider pruning through `peer_store`. `node::async_find_peer(...)`,
  `async_find_providers(...)` and `async_provide(...)` use the iterative path;
  `ADD_PROVIDER` remains libp2p-compatible fire-and-close and validates that
  the provider peer matches the authenticated stream peer. Rendezvous
  registration refresh replaces same peer/namespace records, discover uses
  cookie continuation and namespace scoping, signed PeerRecords are strictly
  validated before learning endpoints, and `node::async_refresh_discovery()`
  feeds fresh DHT/Rendezvous relay-capable peers into the existing AutoRelay
  reservation owner. Live many-peer donor topology artifacts remain limited by
  fixture support and are tracked as donor-matrix gaps, not unsupported runtime
  behavior.
- F.3 implemented checkpoint: connection manager and resource policy live in
  private `forge_net_p2p` host/node orchestration around `session_state`, not in
  `direct`, transport libraries, plugins or product loops. `resource_manager`
  counts pending/established inbound/outbound session scopes and denials;
  `connection_manager` owns admission decisions, protected peer tags,
  low/high-watermark pruning, stale-safe removal and deterministic stop cleanup.
  Public operator/test control is additive: `protect_peer`, `unprotect_peer`
  and `is_peer_protected`.
- F.3 backoff is endpoint-local and policy-backed:
  `dial_backoff_base + dial_backoff_step * failures^2`, capped by
  `dial_backoff_max`. A failed TCP or QUIC endpoint must not poison other
  direct endpoints or fresh relay paths for the same peer.
- F.3 abuse accounting is peer-local. A peer that crosses the malformed-message
  threshold closes only the offending session, updates metrics/backoff and does
  not poison unrelated peers or transports. Protected peers survive pruning, but
  protection does not bypass hard admission when no unprotected session can be
  freed. Mixed QUIC/TCP networks must not become a denial-of-service amplifier.
- F.4 implemented checkpoint: donor docs and
  `tests/libp2p_interop/donor_cases.json` are the support-claim audit surface.
  Block F is closed only for behavior that maps to a libp2p spec/donor source,
  an FORGE component test, the donor matrix and a live Go/Rust artifact where the
  fixture can exercise that behavior. Component-only proofs and fixture gaps stay
  explicit as `known_gap`; they are not treated as live compatibility claims.
- `/ws` and `/wss` remain multiaddr parse/store only. There is no P2P
  dial/listen path for browser/proxy transports in Block F.
- `forge::plugins::net::p2p::node` and focused friend plugins come after core behavior is proven.
  Plugins configure and expose the shared node; they do not implement network
  algorithms.

### Block G: API Transport Foundation

- G.0 implemented checkpoint: live libp2p interop artifacts record scenario id,
  attempt id, fixture command, pid, selected/listen addresses, negotiated
  transport/security/muxer, exit code, timeout class and log tail where a
  fixture command is involved. Runner retry is limited to one fixture-timeout
  retry; protocol, security, identity and negotiation failures are not retried
  or hidden as flakes.
- G.1 implemented checkpoint: `forge_api_core` is the transport-neutral contract layer.
  It owns descriptors, registry/view, frame vocabulary, `frame_dispatcher`,
  codec validation, grouped stream state, max-inflight/deadline checks and
  shared error projection. It must not import `forge_net_transport`, QUIC, P2P, HTTP,
  WebSocket, plugins or product layers.
- G.1 implemented checkpoint: `forge_api_transport` is the API-over-transport
  binding. It owns API frames over `transport::stream` and
  `transport::session`, a concurrent client read loop with pending call map,
  serialized writes, `serve_stream(...)`, `serve_session(...)`, bounded
  concurrency, close/cancel wakeups and typed transport API exceptions.
- `forge.api.quic.binding` and `forge.api.p2p.binding` are policy adapters over
  `forge.api.transport`. QUIC policy stays in `forge_net_quic`; P2P policy stays in
  `forge_net_p2p` as protocol id, known-peer checks and discovery scope.
- `forge.api.websocket.binding` shares `forge::api::core::frame_dispatcher`, but does not import
  `forge.api.transport`, because WebSocket is message-oriented and not a
  `transport::stream`.
- HTTP remains a separate request/response binding.
- G.2 implemented checkpoint: `forge::plugins::net::p2p::node` is a narrow host facade
  over `forge_net_p2p`. It owns lifecycle, config-to-node mapping, local endpoint
  reporting, protocol/API route mounting and typed remote API access. Durable
  queues, application fan-out and raw network diagnostics are outside this host
  facade and move to focused plugins or product layers.
- G.3 implemented checkpoint: `forge::plugins::net::p2p::resolver` is a separate plugin for
  API-over-P2P metadata discovery and compatible remote opening. Product
  plugins publish discoverable APIs through the resolver; it mounts the actual
  API route through `forge::plugins::net::p2p::node`, stores a serializable descriptor projection
  (API id/version, protocol id string, codec, limits, methods and errors), and
  lets clients resolve a compatible API without hardcoded product protocol ids.
  Identify continues to advertise protocol ids; the resolver adds typed FORGE API
  metadata above P2P instead of expanding core Identify semantics. Its network
  metadata protocol `/forge/api/resolver/2` is FORGE-specific and does not extend
  Go/Rust libp2p support claims.
- G.4 implemented checkpoint: `forge::plugins::net::p2p::diagnostics` is a read-only in-process
  plugin for peer/path/session/relay/DHT/Rendezvous/pubsub/connection-manager
  health. `forge_net_p2p` owns immutable diagnostics snapshots; the plugin exposes
  capped operator/test projections through `forge_app`. It does not add a network
  diagnostics protocol, product authorization, remediation, routing policy or
  retry decisions.
- G.5 implemented checkpoint: `forge::plugins::net::p2p::pubsub` is an in-process plugin facade over
  core GossipSub. It offers raw and typed topic publish/subscribe, bounded
  local handlers, handler deadlines, topic allow/deny policy, deterministic
  subscriptions and capped plugin snapshots for application plugins. It is not a
  durable queue, delivery system, authorization layer or replacement for
  `forge_net_p2p` GossipSub mesh/scoring/heartbeat mechanics.
- G.6a implemented checkpoint: observability starts as a reusable FORGE OTLP logs
  exporter. OTLP is OpenTelemetry Protocol. `forge_otlp` exports `forge_log` records
  to an operator-configured OpenTelemetry Collector through OTLP/HTTP JSON over
  `forge_net_http` and `forge_asio`. `forge_log` remains synchronous and transport-free;
  the OTLP sink is an optional adapter with bounded queueing, batching,
  retry/backoff, `Retry-After` handling, `Resource` attributes such as
  `service.name` and deterministic shutdown. Metrics and traces are later
  additive exporters, not log sinks.
- G.6 scope decisions: start with OTLP JSON over HTTP, not a gRPC transport,
  because gRPC and Protocol Buffers runtime dependencies bring a large
  event-loop and packaging cost. Do not vendor-lock FORGE to backend SDKs for
  Sentry, Grafana, Datadog or OpenTelemetry C++; backend routing belongs in the
  Collector.
- G.6b implemented checkpoint: crash capture is separate from the live log sink.
  Signal handlers write only fixed binary records to a bounded local spool with
  async-signal-safe operations. On the next start, `forge_otlp` reads and
  validates the spool, quarantines malformed files, exports safe crash evidence
  through the existing OTLP/HTTP log exporter and deletes files only after a
  successful export. `std::set_terminate` captures typed exception category/code
  and stacktrace addresses before aborting, but never persists raw `what()` text
  or application context.
- G.6 out of scope: native minidump SDK integration, watchdogs for
  SIGKILL/hangs, metrics registry export and trace/span export remain later
  optional blocks.
  Telemetry export is opt-in by config; local spool contents and outgoing
  attributes must respect redaction rules because peer ids, paths and application
  context can be sensitive.
- G.7 optional future checkpoint: `p2p_delivery` may become a separate durable
  async plugin if a product needs store-backed retry. It is separate from the
  host facade, does not promise exactly-once semantics and does not own product
  acknowledgement semantics.
- H.0 implemented checkpoint: Storlane-level products do not need generic
  delivery acknowledgements for ordinary request/response work. Product plugins
  publish typed APIs through `forge::plugins::net::p2p::resolver`, clients resolve compatible
  remotes without hardcoded protocol ids, and domain methods return product
  receipts with idempotency keys. The receipt proves the application-level
  operation result for that protocol; authorization, durable state and
  authoritative settlement remain product/control-plane responsibilities.
- IPFS/Boxo content, provider, exchange, retrieval and pinning donors inform
  future product/content/storage layers. They are not `forge_plugins` or
  `forge_net_p2p` support claims.

AutoNAT, AutoRelay, DHT and pubsub algorithms must live in `forge_net_p2p`.
`forge::plugins::net::p2p::node` configures and runs the shared node, then exposes the
network capabilities through narrow application APIs. If a network behavior is
not implemented yet, expose a typed unsupported/limited result instead of hiding
the gap above the network layer.

## Donor Test Adoption

libp2p specs are the contract for wire behavior. go-libp2p and rust-libp2p
tests are donor criteria, not optional inspiration. FORGE should not copy Go/Rust
runtime code, but it must adopt their golden vectors, scenarios, failure cases
and acceptance criteria.

Use go-libp2p, rust-libp2p and libp2p specs as donor architecture and test
criteria. Copy layering and compatibility expectations, not Go/Rust public API
shape. Builder style is allowed only over normal owner-shaped implementations,
never as a hidden implementation or decorative options bag.

For each supported libp2p protocol, create a traceability matrix:

| Protocol | Spec source | Donor tests inspected | FORGE unit tests | FORGE interop tests | Unsupported gaps |
| --- | --- | --- | --- | --- | --- |
| Multiformats / Peer ID | `libp2p-specs` | go-libp2p/rust-libp2p identity tests | golden byte vectors | not required | listed explicitly |
| QUIC + Ping | `libp2p-specs/quic`, `libp2p-specs/ping` | go-libp2p `test-plans`, rust-libp2p interop tests | FORGE transport tests | FORGE <-> Go/Rust in both directions | listed explicitly |
| Identify | `libp2p-specs/identify` | go-libp2p/rust-libp2p Identify tests | FORGE encode/decode and peerstore tests | FORGE <-> Go/Rust Identify exchange | listed explicitly |

Test layers:

- `golden`: golden vectors for varint, multicodec, multihash, multibase, Peer
  ID, signed records and Identify messages.
- `component`: FORGE-to-FORGE tests for endpoint parsing, negotiation, Ping,
  Identify and peer/path store behavior.
- `interop`: FORGE client/server against go-libp2p and rust-libp2p in both
  directions.
- `plugin/system`: realistic scenarios through `forge::plugins::net::p2p::node` and
  small focused friend plugins, not a parallel fake test runtime.
- `performance/stability`: latency, throughput, long sessions, reconnect, many
  peers, backpressure and peerstore recovery.

If the libp2p ecosystem already has an acceptance criterion, the FORGE test must
reference that criterion. A test that is only "similar to libp2p" is not enough.
A protocol cannot be marked supported until it has spec-derived, donor-derived
and required interop coverage.

## Integration Example

```cpp
auto options = forge::net::p2p::node::options{
   .certificate_pem = certificate_pem,
   .private_key_pem = private_key_pem,
};

auto node = forge::net::p2p::node{runtime, options};
node.register_protocol_handler(
   forge::net::p2p::protocol_id{.value = "/example/1"},
   [](forge::net::p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
      std::vector<std::uint8_t> frame = co_await incoming.stream.async_read_frame();
      co_await incoming.stream.async_write_frame(frame);
   });

boost::asio::awaitable<void> connect_example(forge::net::p2p::node& node) {
   co_await node.async_listen(forge::net::p2p::parse_endpoint("/ip4/127.0.0.1/udp/9443/quic-v1"));
   forge::net::p2p::node::session_info session = co_await node.async_connect(remote_endpoint, {.expected_peer = remote_peer});
   forge::net::p2p::stream stream = co_await node.async_open_protocol_stream(
      session.remote_peer,
      forge::net::p2p::protocol_id{.value = "/example/1"});
   use_stream(std::move(stream));
}
```

The protocol ID identifies a stream contract. The protocol still owns its own
message validation, durable semantics and authorization above FORGE.

## Failure Model

- A failed direct endpoint is one candidate failure, not necessarily whole
  operation failure while deadline budget remains.
- Peer mismatch and TLS verification failure are correctness failures.
- Oversized or malformed control envelopes are rejected before handler dispatch.
- Relay use is explicit and reservation-backed.
- Non-positive timeouts are rejected early.
- `async_connect(...)` establishes a direct session to a concrete endpoint.
  Direct -> hole punch -> relay fallback happens when opening a protocol stream,
  while host plugins expose typed remote APIs over the selected stream.

## Security Boundary

Peer identity is transport identity. It proves which key/certificate completed
the handshake; it does not authorize product actions. Consumers must still
perform their own authorization and policy checks.

## Donor Decisions

Accepted:

- ngtcp2 transport engine.
- libp2p-compatible host/protocol separation without adopting Go/Rust runtime
  architecture.
- libp2p-compatible Identify and Ping.
- libp2p multiaddress as a compatibility format behind an FORGE typed endpoint
  model.
- libp2p Peer ID, multiformats, key encoding and multistream-select as wire
  compatibility requirements.
- Circuit Relay style explicit reservation.
- AutoNAT/AutoRelay as network services, not product/plugin loops.
- DCUtR-style hole punching as a bounded attempt, not magic connectivity.
- Kademlia DHT and rendezvous as discovery donors; component proof and live
  FORGE <-> go-libp2p/rust-libp2p artifacts are tracked in
  `docs/donors/forge-p2p-dht-rendezvous-v1.md`.
- GossipSub v1.1 with v1.0 fallback as a pubsub/gossip donor; proof and
  hardening artifacts are tracked in `docs/donors/forge-p2p-gossipsub-v1.md`.
- Transport abstraction before further discovery/path hardening, so DHT,
  rendezvous, Identify, AutoRelay and path scoring are not built twice around
  QUIC-only endpoint state.
- `forge_net_transport` as a reusable byte-stream/session substrate, not an API/RPC
  framework. A future `forge.api.transport` layer should sit above it and stop
  QUIC/P2P/TCP API bindings from duplicating API frame serve-loop logic.
- TCP + Noise/TLS + Yamux direct path is accepted as the TCP compatibility
  baseline; proof is tracked in
  `docs/donors/forge-p2p-tcp-noise-yamux-v1.md` and
  `docs/donors/forge-p2p-tcp-tls-yamux-v1.md`.
- Chained relay paths are a future extension above the compatible one-hop Relay
  v2 baseline, never a replacement for libp2p Relay v2 semantics.
- Syncthing/libtorrent-style path scoring/backoff.
- OTLP/HTTP JSON as the first observability export path, with one
  operator-configured OpenTelemetry Collector endpoint and reusable `forge_otlp`
  for logs first, metrics/traces later.
- Crash reporting through a local durable spool and next-start resend over the
  same OTLP path. Signal handlers only perform async-signal-safe writes; normal
  symbolication/redaction/export happens outside the crashing signal context.
- Durable asynchronous retry as an application/plugin-level pattern, not a
  storage dependency inside `forge_net_p2p`.
- Typed request/receipt protocols over `forge::plugins::net::p2p::resolver` as the baseline for
  synchronous product operations. Idempotency keys and domain receipts belong to
  the product API contract, while FORGE supplies discovery, stream opening and API
  frame transport.

Rejected:

- Direct libp2p dependency in v1.
- Go/Rust runtime style as FORGE public architecture.
- Free-form tests that do not map to libp2p specs, donor tests or interop
  criteria.
- Product storage/application semantics inside P2P.
- Product authorization or business acknowledgement inside P2P.
- Silent insecure peer identity fallback outside tests.
- Direct backend SDK lock-in for observability, including Sentry native,
  OpenTelemetry C++ or per-backend FORGE sinks that bypass the Collector.
- gRPC as the first OTLP transport path for FORGE runtime telemetry; it can be
  revisited only if HTTP/JSON becomes insufficient and dependency/event-loop
  costs are justified.

## Verification

`test_forge_quic_p2p` covers QUIC handshake, frame codec, ALPN and mTLS failures,
pinned fingerprints, direct protocol streams, peer exchange, relay, reachability,
hole punching, malformed envelopes and production option checks.

Validation note, 2026-06-02: during the P2P identity signature hardening review
checkpoint, one `ctest` run caught a transient `test_forge_quic_p2p` segfault.
The follow-up full binary run (`test_forge_quic_p2p`, 110 cases) and repeated
`ctest` gate passed. Treat any recurrence in CI as a Block E/F hardening flake
blocker and debug it from artifacts before broadening P2P scope.

Known test race, 2026-07-16: a full unrelated Forge validation run completed
89 of 90 tests and failed
`p2p_gossipsub_forwards_between_subscribed_peers` after the subscriber callback
had already delivered the expected payload. The callback fulfills its promise
before returning `validation_result::accept`, while the delivered-message metric
is updated after validation completes. The test can therefore observe a ready
payload future and immediately sample `messages_delivered == 0`. This is a test
synchronization defect, not evidence of a lost message. The test must wait on a
deterministic post-validation/accounting condition; an arbitrary sleep is not an
accepted fix. Track and fix this independently from Contract SDK work as
Forge issue #84.
