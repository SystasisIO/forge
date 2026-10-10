# Stage 7: Official P2P Plugin Integration

## Status

Discussion only. Stage 6 PR12 was merged as
`94714f0ff465435ef317ec1891d44b1e9380c01e` in [PR 171](https://github.com/SystasisIO/forge/pull/171).
This document prepares the next implementation PR; it does not approve concrete
new interfaces, change plugin behavior or promote the stack to production.

The [implementation roadmap](forge-p2p-production-implementation-v1.md) and
[production criteria](forge-p2p-production-hardening-v1.md) remain authoritative.
Apply `create-library` and `create-plugin` before implementation. Keep the
existing `plugins/net/p2p/{node,resolver,pubsub,diagnostics}` leaf ownership;
do not restore `plugins/p2p` or introduce an aggregate plugin.

## Approved Boundaries

- The official node plugin owns configuration, identity, persistence, listeners
  and startup/shutdown. Consumers must not receive a mutable `node&`,
  `shared_ptr<node>` or peer-store accessor.
- Consumers receive focused local typed contracts grouped by coherent roles,
  not a separate interface for each method. Use existing Forge API machinery
  and library types; these contracts are not automatically remote APIs.
- Discovery, dialing, mDNS, AutoNAT, relay, DCUtR, GossipSub and resource loops
  remain inside `forge::net::p2p::node`. The plugin translates configuration,
  supplies dependencies and delegates operations, without a second runtime.
- Publication, subscription and provider-registration ownership is explicit.
  Preserve each library handle's actual cancellation and completion contract;
  do not infer a completed join merely from a cancellation request.
- Stage 7 must classify every public node capability as a consumer operation,
  owner-only configuration/lifecycle, or explicitly deferred capability with
  rationale. Ordinary DHT operations and host events cannot disappear behind
  an owner-only label.

## Current Starting Point

The node plugin currently exports `api`, `diagnostics_source` and
`pubsub_source` from `forge.plugins.net.p2p.node.api`. It already supplies API
publication/connections, diagnostics and ordinary PubSub delegation. It does not
yet expose the complete Stage 6 consumer surface or configuration.

The raw node now includes staged protection, private TCP networking, DNS/dial
policies, reachability/host events, mDNS, AutoRelay, DCUtR and GossipSub scoring
and extensions. Existing DHT/topology and durable-state ownership remain below
the plugin. This implementation inventory is not a whole-stack production claim.

## Decisions Before Implementation

| Area | Required outcome | Still to agree |
| --- | --- | --- |
| Discovery and DHT | Profile-aware lookup, values and owned provider registration | Exact local contract grouping and custom validator/selector contribution |
| PubSub | Full-message and opt-in partial operations with existing token/handle semantics | Consumer versus owner-only tuning, validators and callback contribution |
| Events and diagnostics | Typed snapshots/events, overflow/resynchronization and bounded read-only diagnostics | Interface grouping and subscriber lifetime |
| Connections and publications | Preserve existing publication ownership and authenticated API binding | Whether controlled raw-protocol registration/opening is needed and its ownership |
| Configuration | Map validated protection, PSK, DNS, AutoNAT, mDNS, relay/path and GossipSub policies | YAML grouping, safe defaults and code-only policy contribution |
| Compatibility | Explicit migrations for any changed source/config/API contract | Exact contract/plugin versions after the affected surfaces are known |

Do not serialize arbitrary callbacks into YAML or copy library operational DTOs
into a second plugin model. Secrets use the existing secrets dependency and must
not appear in errors or diagnostics. Incompatible private-network transport
combinations must retain the raw node's fail-fast validation.

UPnP, P2P WebSocket, WebTransport and WebRTC are not additions to this Stage 7 PR.
Swarm keeps its typed Forge API binding; raw streams are not a Swarm bypass.

## Proposed Delivery And Acceptance

After maintainer agreement, deliver the approved plugin surface in one focused
implementation PR, with internal slices for the capability inventory, config
mapping, consumer contracts and parity tests. Do not mix new wire algorithms into
that PR; any discovered core defect must have an explicit regression and scope.

Required exit evidence:

- Complete public-capability coverage table, including the reason for every
  owner-only or deferred operation, with no unsupported capability advertised.
- Raw-node/plugin parity for startup, hydration, restart, configuration errors,
  cancellation and awaited shutdown; no plugin-owned protocol workers.
- Required secrets, invalid profile combinations, safe defaults, quotas and
  backpressure tested through the official plugin path.
- Consumer-handle lifetime, stale-generation rejection, pending operations at
  shutdown and host-event overflow/resynchronization tested through local APIs.
- ObjectDB persistence parity through MDBX and RocksDB where available. Record
  unavailable environments as `NOT_RUN`, never as a successful backend check.
- Independent package consumers, structure/inventory gates, focused integration
  suites and affected live donor scenarios. Build with coordinator-owned `-j4`;
  do not launch the full CI matrix for this discussion document.

The PR12 Rust QUIC limitation remains explicit: original shutdown is
`NOT_PROVEN`; the separately documented local observation patch is not an
unmodified-donor result. Plugin work must not silently remove that distinction.

## Product Integration And Stage 8

After Stage 7 parity gates pass, begin test integration with Spine first, pinned
to an exact Forge commit. Storlane and other developing products can follow
through the same official surfaces. Preserve their unrelated worktrees.

Turn confirmed product failures into reproducible Forge regressions and rerun
the affected gates. Product feedback complements, but does not replace, Stage 8
long-running, hostile-peer, churn and resource-exhaustion testing. Until those
gates pass, describe this as test integration with an evolving stack.

No plugin implementation or product migration is started by this document.
