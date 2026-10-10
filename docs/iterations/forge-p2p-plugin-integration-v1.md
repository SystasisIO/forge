# Stage 7: Official P2P Plugin Integration

## Status

Implementation approved by the maintainer on 2026-10-11. Stage 6 PR12 was merged as
`94714f0ff465435ef317ec1891d44b1e9380c01e` in [PR 171](https://github.com/SystasisIO/forge/pull/171).
Stage 7 implementation is tracked in PR 172 on `forge-p2p-plugin-integration-v1`.
Approval is for the plan below, not a statement that the implementation or
acceptance is complete. This PR does not promote the stack to production.

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

## Approved Consumer Contracts

The interfaces are local Forge API contracts. They are not implicitly remotely
callable, and they all delegate to the same node. Preserve existing library
types, cancellation behavior and operation ownership rather than creating a
parallel plugin DTO or network runtime.

| Interface in `forge::plugins::net::p2p` | Required surface | Ownership |
| --- | --- | --- |
| `node::api` | Local identity/addresses, connection to a peer, API publication and API connections | No shared-node stop, listener or identity mutation |
| `node::dht_api` | Profile-aware find peer/providers, provide, put/get value, typed IPNS record creation | Return the existing provider registration; never expose identity private material |
| `node::configuration_api` | Pre-start connection gater and custom DHT value-policy contributions | Local composition only; reject duplicate/unknown policies and freeze before node startup work |
| `node::protocol_api` | Owned custom protocol registration and stream opening | Protect built-ins/API routes and replacement generations; unregister closes admission, not unrelated existing streams |
| `pubsub::api` | Ordinary/typed publishing and subscriptions, validation, partial-topic registration, advertisement, discovery and send | Preserve immutable registration tokens and distinguish requested cancellation from completed callbacks |
| `diagnostics::api`, additive `diagnostics::events_api` | Bounded node/resource/peer/DHT/relay/PubSub snapshots and existing typed host events | Read-only; reuse the library event subscription and resynchronization semantics |
| `resolver::api`, `resolver::managed_api` | Existing peer API catalog, contract resolution and managed connection | API discovery at known peers, not a second DHT or peer-discovery implementation |

New independently versioned local contracts start at 1.0. Audit existing
contract changes for source/API compatibility and document mechanical migration
and plugin version changes where required. Do not change the Forge release-train
version in this feature PR.

Custom protocol registration uses a move-only `protocol_registration` with
`active()` and idempotent `close() noexcept`. It has no `async_close()` guarantee
for already admitted raw streams. Duplicate registration fails; replacement
requires the active owner token, so a stale or foreign token cannot replace or
remove a newer generation. Reserved built-in protocols and configured DHT
profiles are protected even when a corresponding service is disabled. Check
raw/API route conflicts in both publication paths under consistent ownership.
Remove the old unowned `publish_protocol()` entry point with an explicit node
contract migration rather than retaining a bypass.

Host events use a new local `node::host_event_source` delegated to
`diagnostics::events_api`, both initially 1.0. Preserve the existing diagnostics
source contract instead of adding pure virtual methods to it invisibly. Copy
shared sources under synchronization, invoke outside the lock, and do not add a
second event queue. Preserve native initial snapshots, resynchronization flags,
single-reader admission, cancellation and end-of-stream on shutdown.

## Configuration And Contributions

- Node owns identity and persistence settings, transports, protection/resource
  limits, PSK, DNS/dial policy, topology/DHT, reachability, mDNS, relay and path
  policy. Adapt to the existing validated library options.
- PubSub owns its configuration of versions, scoring, quotas and extensions;
  apply it to the shared node before startup. Resolver and diagnostics retain
  only their own settings.
- Product-specific DHT validation/selection/expiry and connection policy callbacks
  are supplied as typed C++ contributions during composition/initialization.
  Freeze global contributions before node startup, reject duplicates or missing
  required custom policies, and preserve pure validator/selector semantics.
  Reuse the complete `dht::value_policy`, including expiry; never permit a
  product contribution to replace Amino validation.
- Topic callbacks and Partial Messages encoding/reconstruction belong to the
  consumer registration. Forge does not automatically split arbitrary bytes.
- Secrets remain secret-provider references. Private-network incompatible
  transports fail before listeners open; insecure mode remains explicit/test-only.

Do not serialize arbitrary callbacks into YAML or copy library operational DTOs
into a second plugin model. Secrets use the existing secrets dependency and must
not appear in errors or diagnostics. Incompatible private-network transport
combinations must retain the raw node's fail-fast validation.

`configuration_api` accepts one connection gater and one complete native value
policy set per configured custom values-enabled profile. Validate all three
callbacks (`validate`, `select`, `expiry`) and namespace prefixes with the
existing native validator. Configuration keeps profile declarations until
consumer initialization can install the required policies. `begin_startup()`
atomically validates/completes the native profiles and freezes contributions
before its first await, identity reads and P2P persistence opens. This does not
claim to precede the shared DB Store driver's earlier `after_initialize()` open.
Never construct a temporary permissive or values-disabled profile to bypass
missing validation. Global gater callbacks must be nonblocking and thread-safe;
callbacks must retain their own state rather than a raw consumer pointer.

The remaining configuration work is grouped by the existing native owners:

| Plugin owner | Configuration groups to map and test |
| --- | --- |
| Node | Transport/security selection, transport buffers, private-network secret and egress policy |
| Node | Resource scopes, connection admission/pruning, Identify and peer-store bounds |
| Node | DNS resolver/expansion, dial staggering and black-hole detection, path policy |
| Node | AutoNAT v1/v2 client/service, observed addresses, Ping and host-event bounds |
| Node | mDNS families/interfaces/TTL and packet/peer bounds |
| Node | AutoRelay candidates/reservations/renewal and Circuit Relay v2 service quotas |
| Node | Per-profile DHT record/provider policy, bootstrap retries, topology and Rendezvous bounds |
| PubSub | Version preference/fallback, signature policy, flood publish, scoring, IDONTWANT and Partial Messages bounds |

Reuse described native resource-limit types where possible. Other configuration
records express checked units and policy only; they must not duplicate native
runtime state. Do not silently overwrite explicit resource budgets with derived
session limits. Reject unknown policy fields rather than accepting a misspelled
security setting as a warning. Keep application-shell-owned config keys outside
that validation boundary.

Private-network configuration stores only a secret reference. Read it through
the existing Secrets `get_bytes` API with a dedicated purpose, require exactly
32 bytes for the native PSK, and erase temporary buffers on success and failure.
Do not add a file/env secret loader or expose raw secret material in diagnostics.

Peer protection and score contributions use move-only owner tokens with unique
internal native keys. Closing a token removes only its own contribution, never
bootstrap protection or another consumer's tags. Admission is bounded; score
accounting must remain representable for every token-release order, including
mixed positive/negative values. Stop closes new admission and late tokens are
safe after shutdown. This reuses native keyed protection/tagging rather than
creating another connection manager.

UPnP, P2P WebSocket, WebTransport and WebRTC are not additions to this Stage 7 PR.
Swarm keeps its typed Forge API binding; raw streams are not a Swarm bypass.

## Capability Coverage Baseline

This table classifies the current public node surface before implementation.
Tests and exact adapter locations must be added as slices land. A row is a
delivery obligation, not evidence of an implemented plugin feature.

| Raw node surface | Stage 7 disposition | Reason |
| --- | --- | --- |
| `local_peer`, `local_endpoint(s)` | `node::api` | Consumer identity and address information |
| `async_connect` ordinary endpoint/multiaddr overloads | `node::api` | Bounded authenticated connectivity, preserve expected-peer checks |
| API publications and connections | Existing `node::api` / resolver | Preserve generation ownership and authenticated invocation |
| `async_find_peer`, `async_provide`, `async_find_providers`, `async_put_value`, `async_get_value`, `create_ipns_record` | `node::dht_api` | Complete profile-aware data/discovery operations |
| Custom `register_protocol_handler`, `unregister_protocol_handler`, `async_open_protocol_stream` | Owned `node::protocol_api` | Do not export unowned global route mutation |
| `async_subscribe`, `async_unsubscribe`, `async_publish`, all partial operations | `pubsub::api` | Registration-scoped full and partial messaging |
| `metrics`, `diagnostics`, `routing_status`, `lifecycle_state`, `reachability_status`, `host_events`, `pubsub_snapshot`, `pubsub_scores` | `diagnostics::api` with source delegation | Bounded read-only visibility, including event overflow |
| `async_ping` | Consumer connectivity operation | Bounded liveness/latency probe, not a second maintenance loop |
| `protect_peer`, `unprotect_peer`, `tag_peer`, `untag_peer`, `is_peer_protected` | Owner-scoped policy contribution | Consumers must not remove another owner's protection or globally overwrite tags; exact contribution ownership tested |
| `async_refresh_discovery`, `async_request_peer_exchange`, `async_rendezvous_register/discover` | Owner discovery policy / managed node execution | Preserve configured source/namespace bounds; no ad-hoc plugin discovery loops |
| `async_probe_reachability`, relay reserve/refresh/cancel, hole-punch attempt/cancel | Owner policy / existing node managers | Do not let one consumer cancel another's shared relay/upgrade operation |
| `async_connect_coordinated` | Owner-internal path management | Native simultaneous-dial orchestration is not a consumer transport bypass |
| `peers()` | Owner-only | Persistence and mutable peer state are not exposed |
| `set_advertised_endpoints`, `async_set_bootstrap`, `async_listen`, `async_hydrate_peer_state`, `async_start`, `request_stop`, `async_stop`, `stop` | Owner-only lifecycle/configuration | One owner for listeners, signed identity facts, persistence and shutdown |

## Implementation Sequence

One focused implementation PR with sequential bounded slices:

1. Record this coverage baseline and implement profile-aware DHT delegation,
   local contract retrieval, lifecycle rejection and package-consumer coverage.
2. Extend diagnostics with library events and snapshots; extend PubSub with
   partial registrations and scoring/extension configuration without changing
   raw cancellation or completion semantics.
3. Introduce controlled custom-protocol registration/opening and ordinary
   connect/ping operations. Prove route collision, pre-start close, replacement
   and shutdown behavior; do not conflate raw streams with API publications.
4. Complete node policy/configuration mapping and typed pre-start contributions.
   Validate secrets and incompatible settings before network admission.
5. Fill the coverage table with source/test evidence and run raw-node/plugin
   parity, persistence, lifetime, package and affected donor tests.
6. Run independent exact-head reviews, fix valid findings and repeat affected
   validation. Merge into `dev` only after all scoped acceptance gates pass.

The coordinator owns integration and the single `-j4` build tree. Persistent
6.1 Sol xHigh developers/reviewers receive bounded file sets and acceptance
criteria, not the full chat. Only one writer touches the shared implementation
at a time. Remove only inactive task-owned builds after preserving evidence;
never clean neighboring worktrees or active downstream builds.

## Acceptance

Do not mix new wire algorithms into this PR; any discovered core defect must
have an explicit regression and scope.

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
  do not launch the full CI matrix.

The PR12 Rust QUIC limitation remains explicit: original shutdown is
`NOT_PROVEN`; the separately documented local observation patch is not an
unmodified-donor result. Plugin work must not silently remove that distinction.

## Product Integration And Stage 8

Spine integration is a separate maintainer discussion after Stage 7. It is not
authorized or started by this PR. Once agreed and after Stage 7 parity gates
pass, Spine is the first planned test consumer, pinned to an exact Forge commit.
Storlane and other products can follow. Preserve unrelated downstream worktrees.

Turn confirmed product failures into reproducible Forge regressions and rerun
the affected gates. Product feedback complements, but does not replace, Stage 8
long-running, hostile-peer, churn and resource-exhaustion testing. Until those
gates pass, describe this as test integration with an evolving stack.

## Progress

- Plan approved; implementation and acceptance are in progress.
- The first source slice adds local `node::dht_api` 1.0 with profile-aware
  operations and native provider registration/IPNS types. Adapter coroutines
  capture shared ownership before suspension and reject new operations after
  stop. Five focused native-macOS DHT tests passed (97 assertions), including
  a separate reader's network GET_VALUE/GET_PROVIDERS and deferred calls after
  host/API destruction. The package-consumer update is not executed yet.
- Native P2P, MDBX and RocksDB foundation targets built in the coordinator's
  single macOS `build/stage7` tree with `-j4`. This is build evidence, not a
  Stage 7 runtime or donor-compatibility verdict.
- Additive host-event source/diagnostics contracts are implemented without an
  extra queue. Eight focused tests passed (97 assertions), including native
  initial state, canceled reads, stop admission and source/handle lifetime.
  Both independent source reviews found no new issues in this adapter slice.
- The first full native run exposed three system-DNS-dependent fixtures and a
  nonportable closed-port assumption. DNS tests now use the existing local
  authoritative fixture; TCP refusal requires exact native errno/category and
  joined cleanup, not an arbitrary `peer_not_found` error. Initial failed logs
  are retained separately from the corrected run.
- The new event tests exposed a real raw-node startup defect: the published
  host snapshot could retain `bootstrapping` after entering `maintenance`.
  Startup now publishes the transition before returning. A deterministic raw
  regression prevents background callbacks from masking a missing publication.
- After these fixes, the full macOS `test_forge_quic_p2p` suite passed (1,214
  active cases), as did `test_forge_plugins` (109 cases). Structure and source
  inventory checks passed. This is the DHT/events checkpoint, not final-head
  acceptance for the remaining Stage 7 implementation.
- The same macOS checkpoint also passed `test_forge_p2p_managed_remote`
  (10 cases) and `test_forge_plugins_db_store` (68 cases). The latter was built
  with both `FORGE_HAS_MDBX=1` and `FORGE_HAS_ROCKSDB=1`, including the durable
  P2P cache fixtures for both backends. This does not claim Linux execution.
- Review identified existing PubSub adapter races in join completion,
  last-unsubscribe versus a new join, shutdown admission and source lifetime.
  These are now fixed using the existing gate and notification primitives,
  bounded pending reservations and joined cancellation-isolated operations.
  Twenty focused regressions passed (119 assertions), including late native
  cleanup, self-unsubscribe and capture destruction before shutdown completion.
- Full-topic native unsubscribe now permits idempotent local cleanup after
  stop, without network writes or a false claim of joined callbacks. Its native
  two-node regression passed; token-scoped partial unsubscribe remains closed
  after stop. Independent source reviews found no remaining issues in this
  bounded foundation slice.
- The full native control-queue suite initially exposed a pre-existing test
  race: its RESET barrier observed reserved bytes before protocol-open had
  published the stream. The corrected barrier requires the real stream while
  preserving exact failure-count assertions. Both full suites then passed:
  62 control-queue cases and 129 plugin cases. Initial failures are retained;
  this does not substitute for final-head package or donor acceptance.
- Partial-topic facade operations, protocol ownership, policy contributions
  and the remaining configuration mapping are still pending.
- The complete Stage 7 runtime, package and donor gates remain pending; the
  DHT-slice result is not a whole-PR or production-readiness verdict.
- Product migration remains out of scope.
