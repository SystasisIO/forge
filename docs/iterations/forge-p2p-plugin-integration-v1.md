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
`plugins/net/p2p/{node,resolver}` leaf ownership. The maintainer-approved
consolidation below supersedes the original four-plugin layout; do not restore
`plugins/p2p` or introduce an aggregate plugin.

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
- Node also owns consumer PubSub registration/callback lifetime and read-only
  diagnostics/events. Resolver remains an optional application API catalog and
  connection service, not part of native peer discovery.

## Approved Plugin Consolidation

The maintainer approved consolidating PubSub and diagnostics into node within
PR 172. This is an intentional source/package/configuration break, not removal
of their functionality. Standalone `plugins.net.p2p.pubsub` and
`plugins.net.p2p.diagnostics` plugins, targets, components and configuration
sections are removed without aliases. Resolver stays separate.

| Old consumer surface | Canonical replacement |
| --- | --- |
| `pubsub::api` | `node::pubsub_api`, module `forge.plugins.net.p2p.node.pubsub_api` |
| `diagnostics::api` | `node::diagnostics_api`, module `forge.plugins.net.p2p.node.diagnostics_api` |
| `diagnostics::events_api` | `node::events_api`, module `forge.plugins.net.p2p.node.events_api` |
| Separate PubSub/diagnostics descriptors and package components | Node descriptor and `plugins_net_p2p_node` component |
| `plugins.net.p2p.pubsub` / `plugins.net.p2p.diagnostics` config | `plugins.net.p2p.node.pubsub` / `plugins.net.p2p.node.diagnostics` |

The public `pubsub_source`, `diagnostics_source` and `host_event_source`
intermediaries disappear. Do not preserve them under different public names or
use a second registry hop inside node. The main `api.cppm` owns only `node::api`;
each additional interface owns its matching module. New contract IDs use the
node leaf and role suffix and start at 1.0. Existing unaffected contracts retain
their versions. The node plugin moves from 7.0.0 to 8.0.0 for this approved
consolidation; the Forge project version is unchanged.

Preserve ordinary and typed publishing/subscription, per-consumer callbacks,
quotas, filtering, validation, Partial Messages and immutable registration
tokens. Move the existing callback/registration machinery into one coherent
private PubSub component with exact header/source pairing and justified aspect
units. Native protocol workers, score engine and queues remain in the raw node.
Do not flatten this machinery into `plugin_impl.hxx` or recreate a fake public
source contract solely to retain old test doubles.

Read-only APIs capture shared native-node ownership before delegation and invoke
outside lifecycle/configuration locks. Snapshots keep configured default bounds
and explicit per-call bounds, peer filtering, typed not-found/profile errors and
owned return values. Events retain native initial snapshot, overflow/resync,
cancellation and shutdown behavior, without another queue.

Diagnostics bounds may be updated through validated node configuration after
startup only when every frozen node/PubSub setting remains structurally equal.
Reject mixed or invalid changes atomically, retain typed contributions, and do
not rebuild the native node. An admitted read keeps its captured bounds; a later
read observes the new defaults. Explicit per-call bounds remain independent.
Stopped configuration cannot reopen admission. Read-only APIs gain no setters.

Stop closes PubSub admission and cancels its owned operations. Awaited shutdown
joins callbacks and native operations, disposes callback captures outside locks
and releases the node/persistence only afterwards. Concurrent or delayed API
objects must remain safe. Any necessary sequencing change must have a regression
for blocked operations, self-unsubscribe, cancellation and failed startup.

Migrate real-node and private-component tests rather than delete source-double
coverage without replacement. Installed node consumers exercise all three new
modules; static/package gates reject the removed components/contracts. Update
active catalogs, inventory owners and documentation without rewriting historical
release notes. Wire formats, libp2p negotiation and persisted schema do not change.

The private registration component may receive immutable native operation
dependencies, with production defaults delegating to the retained native node.
This is not an installed API, registry provider or alternate networking runtime.
Its tests keep failures after native mutation, joined cancellation, independent
waiters, callback retirement and Partial downgrade recovery reproducible.
Real authenticated transport tests still use the official configured node.
Tests of the retired source-adapter callback hop are replaced with direct native
read/lifetime tests; no external snapshot callback remains in that path.

## Pre-Stage-7 Starting Point

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
| `node::configuration_api` | Pre-start connection gater, custom DHT value policies and native PubSub options/callbacks | Local composition only; validate before publication, reject duplicate/unknown policies and freeze before node startup work |
| `node::protocol_api` | Owned custom protocol registration and stream opening | Protect built-ins/API routes and replacement generations; unregister closes admission, not unrelated existing streams |
| `node::pubsub_api` | Ordinary/typed publishing and subscriptions, validation, partial-topic registration, advertisement, discovery and send | Preserve immutable registration tokens and distinguish requested cancellation from completed callbacks |
| `node::diagnostics_api`, `node::events_api` | Bounded node/resource/peer/DHT/relay/PubSub snapshots and existing typed host events | Read-only; reuse the library event subscription and resynchronization semantics |
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

Host events use local `node::events_api` 1.0, delegated directly to the owned
native node. Capture shared ownership under synchronization, invoke outside the
lock, and do not add a second event queue. Preserve native initial snapshots,
resynchronization flags, single-reader admission, cancellation and end-of-stream
on shutdown.

## Configuration And Contributions

- Node owns identity and persistence settings, transports, protection/resource
  limits, PSK, DNS/dial policy, topology/DHT, reachability, mDNS, relay and path
  policy. Adapt to the existing validated library options.
- Node owns PubSub versions, scoring, quotas and extensions and diagnostics
  snapshot bounds in separate configuration records. Apply native policy before
  startup. Resolver retains only its own settings.
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
| Node PubSub configuration | Version preference/fallback, signature policy, flood publish, scoring, IDONTWANT and Partial Messages bounds |

Reuse described native resource-limit types where possible. Other configuration
records express checked units and policy only; they must not duplicate native
runtime state. Do not silently overwrite explicit resource budgets with derived
session limits. Reject unknown policy fields rather than accepting a misspelled
security setting as a warning. Keep application-shell-owned config keys outside
that validation boundary.

Sparse resource configuration must preserve the native baseline for every
unspecified budget. The current generic nested decoder constructs a new member,
so directly decoding a partial native scope would replace its finite parent
defaults with unlimited scope defaults. Use explicit optional budget patches
for this boundary instead of changing generic schema semantics in Stage 7.
Apply native defaults, existing derived session defaults, then explicit patches.
An explicit zero is a denial policy, not an absent value. Test empty scopes,
cross-scope isolation, nested misspellings and rejection without mutation.

Prepare immutable configuration candidates outside the configuration mutex.
Capture the previous snapshot under synchronization and recheck its identity,
lifecycle phase and stop admission before a nonthrowing commit. A concurrent
change must cause a typed conflict, not overwrite newer contributions. Copies
and destruction of application callbacks, native construction and registry
shutdown must occur outside that mutex. Startup and stop share the same
admission transition; freeze does not close live protocol or peer tokens.

Dependency binding is narrower than policy freeze. After `initialize()`, a new
configuration cannot require a Secrets or Store provider that was not bound.
Secret references may still change or be cleared before startup when the
Secrets provider is already available. At entry to `after_initialize()`, before
its first await, bind the exact peer-store name, including an empty name.
Reject a different name thereafter; keep accepting valid pre-start policy
changes. Binding rejection must preserve both configuration and route claims.

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
| `async_subscribe`, `async_unsubscribe`, `async_publish`, all partial operations | `node::pubsub_api` | Registration-scoped full and partial messaging |
| `metrics`, `diagnostics`, `routing_status`, `lifecycle_state`, `reachability_status`, `host_events`, `pubsub_snapshot`, `pubsub_scores` | `node::diagnostics_api` / `node::events_api` | Bounded read-only visibility, including event overflow |
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
   Consolidate PubSub/diagnostics ownership and contracts as approved above
   before completing the remaining configuration slice. Preserve previously
   tested behavior and add merged-owner shutdown/package regressions.
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
  host/API destruction. The installed node package consumer subsequently passed
  at the Partial-facade checkpoint, including the DHT and host-event contracts.
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
- At that checkpoint, protocol ownership, policy contributions and the remaining
  configuration mapping were still pending.
- The native prerequisite for Partial facade ownership now supports exact-token
  downgrade to a full-only topic without deleting the full handler, mesh,
  scoring or cache. Token equality preserves owner identity after expiry but
  does not imply admission. Review caught and corrected early broadcast exit
  on a peer-local failure and missing cancellation of an actual native write.
- The native Partial suite passed 17 cases (493 assertions), including a reset
  at the first recipient followed by full delivery from the healthy recipient,
  and caller/node stop during a credit-blocked Yamux write. A full-suite run
  exposed a one-sided subscription barrier in the new mesh fixture; it now
  waits for both directions before its single manual heartbeat. The corrected
  case passed 20 consecutive runs and the complete suites passed again:
  70 control-queue cases and 129 plugin cases. Initial failure evidence is
  retained. This is native adapter groundwork, not a completed plugin facade
  or a new live donor result.
- The plugin Partial facade now delegates enable/disable, advertisement,
  discovery and sends using native generation tokens and required full-message
  fallback. Pending fallback handlers count toward limits; cancellation reaches
  native sends without holding the topic transition gate across their I/O.
  PubSub API/plugin advance to 2.0/2.0.0, the node PubSub source to 2.0 and the
  node plugin to 7.0.0. The main node contract is unchanged in this slice.
- Independent review found that canceling a reserved Partial enable could leave
  the last ordinary native subscription alive. The transition now distinguishes
  a queued reservation from an attempted or committed native owner. The focused
  regression passed 20 consecutive runs; temporarily restoring the old predicate
  reproduced all three leak assertions, after which the fix was restored.
- The corrected Partial facade passed 17 cases (100 assertions), including
  real TCP delivery after downgrade. Both complete suites passed again:
  70 native control-queue cases and 146 plugin cases. Structure (2,368 sources),
  inventory and the independent installed PubSub package consumer passed.
  Initial import/package-boundary and test-barrier failures remain in separate
  logs; these were not counted as successful runs. This is another bounded
  checkpoint, not whole-PR acceptance.
- The installed node and diagnostics package consumers also passed at the
  Partial-facade checkpoint. Their inactive consumer build directories were
  removed after preserving results; the single coordinator build tree remains.
- The raw-protocol slice replaces unowned `publish_protocol` with local
  `protocol_api` 1.0 and move-only registration tokens. The main node contract
  advances to 3.0; API-publication cancellation/drain remains unchanged. One
  registry protects raw/API names, built-ins and all configured DHT profiles.
  Sixteen focused cases passed (332 assertions), including real TCP/TLS/Yamux
  old-stream survival and deterministic install/remove/attach/stop failures.
- Review found that rejected configuration could retain changed native options
  while keeping old reservations. Candidates are now fully prepared before
  reservation commit, followed only by checked nonthrowing state exchanges.
  Two regressions verify startup, old DHT/options, route survival, name reuse
  and unchanged quotas after rejection. Private registry declarations are
  owned by their exact header, not the public plugin interface.
- This slice's initial compiler/fixture-linker and strict structure failures
  are retained separately from corrected results. Ordinary connect/ping,
  scoped peer contributions, configuration completion and configured-path
  donor evidence are still required before merge.
- The corrected protocol slice passed the full plugin (162 cases), managed
  remote (10), DB Store (68) and native PubSub control-queue (70) suites on
  macOS. Both persistence backends remained enabled. Structure (2,380 sources),
  inventory and the installed node package consumer passed. The six registry
  fault/race cases also passed 20 consecutive runs. Both independent reviewers
  reported source-clean for this frozen slice, not for the complete PR.
- The complete Stage 7 runtime, package and donor gates remain pending; the
  DHT-slice result is not a whole-PR or production-readiness verdict.
- Ordinary connect/ping now retain adapter ownership before suspension and
  delegate native authentication, cancellation and deadlines. Move-only peer
  contributions own individual protection/tag keys, bounded by native topology
  limits; closing one owner does not remove another owner's contribution.
  Plugin destruction closes admission but does not replace awaited shutdown.
- The peer checkpoint passed 11 focused plugin cases (99 assertions), native
  connection-manager coverage (15 cases, 113 assertions), and secure plugin
  expected-peer rejection through both MDBX and RocksDB (10 assertions each).
  The secure fixtures use Secrets, ObjectDB and ordinary TLS/Yamux, not insecure
  test mode. Initial insecure-fixture and overly specific exception-type
  failures are retained; neither was counted as authentication evidence.
- All six corrected checkpoint suites passed: plugins (173 cases), managed
  remote (10), DB Store (70), native PubSub control-queue (70), structure
  (2,387 sources) and inventory. A cold Partial awaitable now expects exact
  cancellation after facade destruction closes admission, not an uninitialized
  facade. Both independent reviewers confirmed this semantic correction and
  the peer slice. Configured score-aware pruning still requires its own real
  regression after policy mapping; active tokens alone do not prove pruning.
- The installed node consumer passed after explicit configure/build/run.
  The chain consumer also passed with the new node contract. CMake 3.31.6
  CTest build-and-test does not propagate the intended build parallelism to
  nested Ninja: an over-parallel consumer build was stopped and resumed with
  explicit `cmake --build ... -j4`. Subsequent consumers use that explicit
  workflow. Inactive chain/node consumer builds (about 8.4 GB) were removed;
  no neighboring worktree or active build was cleaned.
- Native policy preflight now validates limits, profile callbacks and protocol
  names without supplying identity, protector or persistence. It reuses native
  peer-store, private-network, DNS, PubSub and QUIC validation. Full validation
  still requires real production resources; key parsing remains node-owned.
- Review caught two QUIC preflight gaps: limits were validated only when opening
  the transport, and the initial fix used a capability bit that does not gate
  actual transport registration. The corrected predicate matches the existing
  public/private transport registry. Fourteen focused cases (147 assertions)
  and all seven checkpoint suites passed: native QUIC/P2P, plugins, managed
  remote, DB Store, PubSub control queue, structure and inventory. Both reviewers
  reported source-clean for this corrected slice. This is not configured-path
  interop or whole-Stage-7 acceptance.
- Configuration now has immutable snapshots and local `configuration_api` 1.0
  contributions for a connection gater and complete custom DHT value policies.
  Declaration validation does not authorize incomplete native startup. Provider
  binding and the exact Store name freeze independently from pre-start policy
  changes; candidate copies, callback retirement and native construction stay
  outside the configuration mutex.
- Review found two shutdown cancellation gaps: cancellation while waiting for
  setup, and cancellation from a retiring raw callback before the protected
  child started. Stop initiation and the joined cleanup now run in one owned,
  cancellation-isolated child without changing the caller's cancellation
  policy. Eighteen focused cases passed (81 assertions). Actual native gater
  rejection and a successful connect/ping control are covered, but the explicit
  insecure fixture is not identity evidence.
- The real Secrets/ObjectDB preflight regression passed 19 assertions: missing
  custom policy causes no Secrets reads or additional adapter transactions,
  while supplying the complete policy permits real startup. All six plugin,
  managed-remote, DB Store, native PubSub control-queue, structure and inventory
  checkpoint suites passed. Both reviewers reported source-clean for this
  corrected configuration slice, not for the whole PR.
- Initial configuration test failures remain recorded: a moved-from producer
  retained a callback capture, and an older test expected custom value policies
  to fail at configure instead of startup. Both tests now enforce the intended
  ownership/composition contract. The installed node consumer passed before the
  final shutdown-callback fix; public package interfaces did not change in that
  fix. Its inactive 2.5 GB build was removed after the successful run.
- The combined native QUIC/P2P run failed in two DHT lookup cases. An isolated
  run passed, but a subsequent repetition failed on iteration five. This is an
  open acceptance investigation, not a load-only explanation or a suite PASS;
  no timeout was increased to hide it. Full configuration mapping, PubSub hooks,
  diagnostics completion and configured-path donor evidence also remain open.
- Product migration remains out of scope.
- The DHT investigation found uncontrolled resolution of synthetic advertised
  names. Explicit local NXDOMAIN fixtures passed 20 consecutive runs of the
  same two cases, 33 assertions per run, with actual DNS query logs. No
  deadlines, provider-count requirements or original assertions changed.
- Donor review distinguishes Go's batch resolution from Rust's concurrent root
  futures. The diagnostic immediate-numeric RED tested additional behavior,
  not the existing Forge contract. Stage 7 preserves batch resolution and its
  known DNS-delay limitation, with explicit batch/deduplication and joined
  cancellation regressions. Full-suite validation is still required; this
  checkpoint is not a new transport or production-readiness claim.
- The corrected DNS checkpoint subsequently passed the full native QUIC/P2P,
  structure and inventory suites. The explicit batch-resolution regressions
  passed both root orders and joined cancellation (58 assertions). No production
  resolver behavior changed.
- Diagnostics source/API 3.0 now delegate per-profile routing counts and native
  PubSub scores. Diagnostics plugin is 3.0.0; events contracts remain 1.0 and
  the unreleased node plugin remains 7.0.0. Returned snapshots own their data,
  source callbacks run outside the lifecycle mutex, and admitted synchronous
  reads may complete after diagnostics shutdown without extending admission.
- Fifteen focused cases passed (267 assertions), including actual routing,
  nonzero native score sampling, typed profile errors, retained values and
  barrier-controlled shutdown. The fixture uses authenticated TCP with test
  memory persistence; it is not the configured production donor fixture.
  All six plugin, managed-remote, DB Store, PubSub control-queue, structure and
  inventory suites passed. Both independent reviews reported source-clean for
  this frozen diagnostics slice, not the whole PR.
- Node policy mapping, complete PubSub policy/hooks and configured-path donor
  acceptance remain open. No source inventory classification is promoted by
  these local adapter tests.
- The approved consolidation now lives in node: `pubsub_api`,
  `diagnostics_api` and `events_api` replace the standalone plugins and public
  source mediators. Resolver remains separate. Node plugin version is 8.0.0;
  the main contract remains 3.0 and the Forge project version is unchanged.
- Review found and fixed a whole-plugin shutdown regression: a failed native
  close was cached permanently instead of allowing a later external retry.
  Immutable per-attempt results now preserve concurrent waiters, keep failed
  registry/persistence owners, and permit a later cleanup attempt. PubSub
  disposal errors remain sticky without preventing other cleanup.
- The consolidation checkpoint passed all seven suites: plugins (196 cases),
  managed remote, DB Store with local MDBX and RocksDB, descriptor surfaces,
  native PubSub control queue, structure and inventory. Installed node,
  resolver and descriptor consumers also built and ran; node was relinked and
  rerun after the final production changes. Removed package components are
  explicitly rejected by the installed node consumer.
- The new official-plugin regression requires an actual TLS/Identify/Yamux
  session and PubSub delivery before checking shutdown. Independent barriers
  prove it waits for both the active callback and its capture destructor. This
  unsigned-message, memory-persistence fixture is not configured production
  persistence or external Go/Rust evidence. The durable-cleanup regression
  separately uses real ObjectDB transactions with one failed flush and proves
  shared failure, retained ownership and successful external retry.
- Initial failed runs are retained: an unauthenticated diagnostics seed did
  not create peer records, old exception/no-op-config expectations no longer
  matched the new contracts, and a new TCP fixture lacked private identity
  material. Corrections use authenticated peer records, precise new contract
  assertions and real Secrets-supplied TLS identities, not production bypasses.
  Both independent scoped source reviews reported no remaining findings.
- Completed task-owned package consumer builds were removed after checking
  for active processes; the shared Stage 7 build remains for subsequent work.
  The next slice maps native resource scopes with sparse optional overrides,
  preserving unspecified defaults and explicit zero-denial budgets. Full
  policy mapping, configured donor parity and whole-PR acceptance remain open;
  this checkpoint is not merge approval or a production-readiness claim.
