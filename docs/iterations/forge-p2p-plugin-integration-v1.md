# Stage 7: Official P2P Plugin Integration

## Status

Implementation approved by the maintainer on 2026-10-11. Stage 6 PR12 was merged as
`94714f0ff465435ef317ec1891d44b1e9380c01e` in [PR 171](https://github.com/SystasisIO/forge/pull/171).
Stage 7 implementation is tracked in PR 172 on `forge-p2p-plugin-integration-v1`.
Approval is for the plan below, not a statement that the implementation or
acceptance is complete. This PR does not promote the stack to production.

The maintainer also approved one static `plugin::descriptor(...)` factory for
each official plugin. Import the owning `...plugin` module. The former free
`descriptor(...)`, `default_descriptor()` and descriptor-only modules are removed
without aliases. Signer and wallet construction options remain parameters of
that same factory; default construction uses their default arguments. Plugin
identity, dependencies, enablement and factory behavior are unchanged. This is
a source/module migration, not a plugin runtime, wire or persistence change.

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

The maintainer additionally approved a single plugin exceptions set and a
canonical public value/configuration layout. `node.exceptions` owns the only
plugin error category; `node.config` owns declarative settings, enums and schema
declarations, with definitions in its paired `.cpp`; `node.types` owns consumer
DTOs without a schema/config dependency. Retire role-specific types/exceptions
modules without aliases. Keep coherent role API and ownership-token modules,
not one large API facade. The original node error numbers 1--3 remain stable;
the README specifies the consolidated PubSub/diagnostics code migration.

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
| Node | Sparse resource scopes and counters; then connection admission/pruning, Identify and peer-store bounds |
| Node | DNS resolver/expansion, dial staggering and black-hole detection, path policy |
| Node | AutoNAT v1/v2 client/service, observed addresses, Ping and host-event bounds |
| Node | mDNS families/interfaces/TTL and packet/peer bounds |
| Node | AutoRelay candidates/reservations/renewal and Circuit Relay v2 service quotas |
| Node | Per-profile DHT record/provider policy, bootstrap retries, topology and Rendezvous bounds |
| Node PubSub configuration | Version preference/fallback, signature policy, flood publish, scoring, IDONTWANT and Partial Messages bounds |

Deliver these groups as bounded checkpoints within this PR, not additional
public config modules. Transport/security precedes private-network provisioning;
PSK provisioning must include dependency binding and secret cleanup, not only
schema fields. DNS/dial/path, reachability, mDNS, relay, DHT/lifecycle and PubSub
then follow their existing native owners. Native mDNS currently provides an
interface-count bound, not an interface-selection API: do not advertise a
selector that the node cannot implement.

The maintainer approved a donor-style configuration composition refinement:
the root groups policies by owner rather than accumulating flat per-protocol
members. Kubo's `config/swarm.go` composes relay client/service, transports,
connection and resource managers; Go host options delegate detailed policies
to those components. Lotus's deliberately smaller operator surface also shows
that every internal constant need not become a YAML setting. Forge keeps
reasonable defaults and sparse overrides, while exposing supported deployment
choices and resource bounds with demonstrated native enforcement. Do not add
duplicate knobs, runtime records, callbacks or undocumented protocol bypasses.
Grouping is not permission to remove supported functionality: preserve existing
thematic YAML behavior, migrate intentional C++ record changes, and keep typed pre-start
contributions for application-owned validators and callbacks. New config
records stay in the canonical `config` module, with metadata after declarations.
The four flat bootstrap keys migrate to `bootstrap-policy.requirement`,
`startup-budget-ms`, `connect-timeout-ms` and `max-parallel`; their values/defaults
are unchanged and the root `bootstrap` array stays in place. The README records
the mechanical migration. Old keys are rejected without compatibility aliases.

The DHT checkpoint must also reconcile record-store capacities: current native
profile construction passes persistence and derives only per-key provider and
record-size limits. Adding unused YAML capacity fields would not satisfy Stage 7.
Pass each supported bound to both operational store and persistence adapter and
test enforcement, or explicitly record a remaining acceptance gap. PubSub's
typed `set_pubsub_options()` is a complete native snapshot contribution; adding
declarative settings must not silently redefine it as callbacks-only.

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

Connection, Identify and peer-state settings follow the same sparse boundary in
the canonical `node.config` module. `connections` patches session pruning and
dial backoff after the existing `max-sessions`-derived low watermark; `identify`
patches the native message/timeout limits and advertised version strings;
`peer-state` patches bounded operational storage without exposing persistence.
Omitted fields preserve native defaults. Explicit values use checked native-size
and duration conversions and the existing node/peer-store validators, not a
second plugin policy validator. A zero grace period is meaningful, whereas zero
Identify and peer-state bounds remain invalid. Tests must establish actual
configured pruning/protection, received Identify facts and retained peer-state
bounds in addition to schema mapping. These settings freeze at startup together
with the rest of the native policy; rejected updates preserve the full snapshot.
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

The following source-to-test map records the current implementation, not a
blanket acceptance result. Paths are repository-relative. Configured live donor
evidence is tracked separately; native tests cannot substitute for that layer.

| Consumer/owner boundary | Adapter/component | Focused regression source |
| --- | --- | --- |
| Peer identity, connect, ping | `plugins/net/p2p/node/api_impl*.cpp` | `tests/plugins/p2p_peer_api_tests.cpp` |
| DHT and local provider ownership | `plugins/net/p2p/node/dht_api_impl.cpp` | `tests/plugins/p2p_dht_api_tests.cpp`, `tests/plugins_db_store/p2p_dht_policy_tests.cpp` |
| Custom protocol admission and generations | `plugins/net/p2p/node/protocol_api_impl.cpp`, private `protocol_registry` | `tests/plugins/p2p_protocol_api_tests.cpp`, `tests/plugins/p2p_protocol_registry_tests.cpp` |
| Consumer peer tags/protection | `plugins/net/p2p/node/peer_contribution.cpp` | `tests/plugins/p2p_peer_contribution_tests.cpp` |
| Read-only snapshots and events | `plugins/net/p2p/node/plugin_diagnostics_api_impl.cpp`, `plugin_events_api_impl.cpp` | `tests/plugins/p2p_host_event_api_tests.cpp` |
| PubSub subscriptions, partial callbacks and shutdown | `plugins/net/p2p/node/plugin_pubsub_api_impl*.cpp`, private `pubsub_service` | `tests/plugins/p2p_pubsub_lifecycle_tests.cpp`, `p2p_pubsub_partial_tests.cpp`, `p2p_pubsub_shutdown_tests.cpp` |
| Immutable pre-start policy contributions | `plugins/net/p2p/node/configuration_api_impl.cpp`, private `configuration_snapshot` | `tests/plugins/p2p_configuration_api_tests.cpp` |
| Secrets, hydration and persistence | `plugins/net/p2p/node/plugin.cpp`, private ObjectDB adapters | `tests/plugins_db_store/p2p_peer_state_persistence_tests.cpp` |
| Configured discovery/reachability/relay owners | `plugins/net/p2p/node/configuration_policy_lifecycle.cpp`, `configuration_policy_services.cpp` | `tests/plugins/p2p_lifecycle_config_tests.cpp`, `p2p_service_config_tests.cpp`, `tests/plugins_db_store/p2p_service_policy_tests.cpp` |
| Configured PubSub options and bounds | `plugins/net/p2p/node/pubsub_policy*.cpp` | `tests/plugins/p2p_pubsub_config_tests.cpp`, `p2p_pubsub_scoring_config_tests.cpp`, `tests/plugins_db_store/p2p_pubsub_policy_tests.cpp` |

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
- A subsequent repeated plugin run exposed a fixture readiness race before
  shutdown. Labeled reproduction identified `await both actual mesh edges`:
  `rpc_read` tracing occurs before applying control messages and is not a
  post-mutation mesh notification. The fixture now checks actual mesh state
  with bounded Asio timer waits, using a 4-second internal deadline inside the
  unchanged 5-second owner join. No production path, delivery assertion or
  shutdown barrier changed. The failed original/labeled runs are retained;
  both independent source reviewers accepted the correction, and ten focused
  real-TCP repetitions passed (23 assertions each).
- The resource/module checkpoint now consolidates plugin errors into one
  `node.exceptions` category and public values into `node.config` (249 lines)
  and `node.types` (97 lines). Schema definitions are out-of-line; private
  mapping and parsed state keep exact pairs. Public modules decrease from 17
  to 14 without removing role APIs or ownership tokens. The source gate rejects
  reintroduced role-specific config/types/exceptions modules and extra plugin
  error categories, with 13 positive/negative gate cases.
- Sparse resource configuration maps nine scopes, eight budgets per scope and
  four operational counters. Eight focused tests passed 11,943 assertions:
  default preservation, explicit zero, YAML and invalid inputs, rollback,
  startup freeze, one error category and native connection denial. A real
  Secrets-supplied TLS/Identify/Yamux connection is the positive contrast; the
  denied operation retains native backpressure and releases reservations.
  Three authenticated peer records separately prove diagnostics rollback and
  live bound changes. Persistence is explicitly in-memory in these fixtures;
  they are not production-persistence or Go/Rust acceptance.
- All eight scoped suites passed three consecutive runs each: plugins (204
  cases), managed remote, DB Store with local MDBX/RocksDB, descriptor surfaces,
  native PubSub control queue, structure, network-plugin structure and inventory.
  Installed node, resolver and descriptor consumers built and ran. Package
  consumers used a fresh dev-component install, without retired modules.
  Builds used the coordinator's one tree with `-j4`; full CI was not run.
- Both independent source reviewers closed the resource/module findings. Fixes
  included the exact `parsed_policy.hxx` owner, cleanup joining before reporting
  exceptions, an isolated YAML typo negative and an observable mixed-update
  rollback. Initial build failures (a changed consumer import during dependency
  scanning, a missing explicit test import and Boost.Test member-pointer
  decomposition) are retained; no production bypass or weaker assertion was
  introduced. Narrow-`size_t` overflow coverage is conditional, not a claim of
  a 32-bit runtime run.
- Next are connection admission/pruning, Identify and peer-state bounds, then
  the other native policy groups above. Configured-path Go/Rust evidence and
  whole-PR acceptance remain open. No donor support-state promotion, project
  version change or merge approval follows from this checkpoint. Task-owned
  package consumer builds were removed; the active Stage 7 build is retained.

### Host Policy And Descriptor Checkpoint

- Sparse connection, Identify and peer-state settings now map 29 numeric
  fields and two version strings to existing native policies. Native validation
  remains authoritative; persistence ownership and unspecified defaults survive.
  Millisecond conversion is checked, clock representability is validated and
  deadline addition saturates without introducing another time type.
- Nine focused configured-path cases passed 2,149 assertions: all-field mapping,
  rejection, rollback, startup freeze, actual TLS/Identify version exchange,
  oversized remote facts, peer-state capacity and protected-peer pruning.
  These fixtures explicitly use memory persistence, not production storage or
  external Go/Rust processes.
- Three native clock/backoff cases passed 35 assertions. A broader filtered
  native run executed 95 cases with 1,956 assertions; other tests were skipped,
  so this is not a full native suite result.
- Six affected suites passed: plugins (213 cases), descriptor surfaces,
  chain signer, crypto wallet, managed remote and DB Store (MDBX/RocksDB).
  Structure, network-plugin structure and inventory passed; the structure
  checker has 23 focused positive/negative regressions.
- All eight official plugin classes now own one static
  `[[nodiscard]] plugin::descriptor(...)` factory. Seven free factories and
  six descriptor-only module/source pairs were removed; wallet was already
  static. Construction options, dependencies and defaults remain unchanged.
  The installed descriptor consumer checks every factory and independent
  instances. Stage 7's public node/resolver module counts are now 13 and five.
- Installed descriptor and node consumers both built and ran successfully
  from a fresh dev-component install. The node consumer exercises the new
  config types and schema definitions; no retired descriptor modules remain
  in the install. Completed consumer build trees were removed after their
  processes exited. The active Stage 7 build and validation logs are retained.
- Project rules and skills require declarations before the final reflection,
  serialization and schema metadata block. The structure gate handles schema
  specializations in either qualified or owning-namespace form and rejects
  reintroduced descriptor wrappers.
- Scoped donor/native and ownership/layout reviews found no remaining source
  issues after fixes. Compiler checks also caught missing explicit imports and
  an incorrect test helper name; the endpoint deadline test was made portable
  across differing system/steady clock ranges. No assertion was bypassed.
- Remaining work is the other native policy groups, followed by configured
  Go/Rust acceptance and whole-PR review. The next slice must distinguish TCP
  security from the currently QUIC-applied transport limits; do not advertise
  those limits as already controlling Yamux or private-network transport.

### TCP Security And QUIC Checkpoint

- `tcp.security` selects the existing TLS/Noise negotiation policy; five sparse
  `quic` settings map connection, stream, outbound-data and inbound-packet queue
  limits. Unknown settings, invalid enum values, zero/overflow and incompatible
  TLS-only relay configuration fail before mutation. Both groups freeze at
  startup and preserve unspecified native defaults.
- Bounds retain their native ownership: listener connections, local stream-open
  admission and advertised incoming credit, and connection-owned queues. These
  are not Yamux limits, application receive-buffer limits or a combined
  bidirectional stream ceiling. The unenforced native `max_frame_size` knob is
  deliberately not exposed through P2P configuration.
- Native QUIC validation now rejects stream credit above `2^60`. A separate
  native race was reproduced: four remote-credit waiters bypassed the local
  limit after a real `MAX_STREAMS` update, creating five active streams under a
  limit of two. Rechecking admission on each allocation-loop iteration fixes
  it. The unchanged regression failed on the old implementation, then passed
  all 54 assertions and five additional repetitions after the fix. No test-only
  production bypass, wire change or donor patch was introduced.
- Fourteen focused configured-path cases passed 438 assertions. TCP covers
  seven compatible selection pairs, both disjoint directions, actual identity,
  Identify/Ping, rollback and freeze. Three QUIC cases use the official app,
  Secrets and DB Store plugins with an Object layer and insecure mode disabled:
  authenticated QUIC
  still works under TCP Noise-only selection; queue denial recovers; stream
  denial/reset/reuse retain the existing connection. Listener saturation leaves
  the first connection usable; its subsequent shutdown releases the slot for a
  replacement peer.
  Their ObjectDB driver is explicitly in-memory, not a disk-persistence claim.
- The initial QUIC fixture incorrectly enabled insecure mode; it was replaced
  with the real configured app rather than weakening authentication assertions.
  The stream fixture also incorrectly equated `identified` with native idle:
  initial Identify Push can still occupy a slot. Filling the four retained
  application slots now retries only typed backpressure under one deadline;
  the fifth denial and reset/replacement assertions remain strict. Cleanup joins
  the fixture scheduler even when plugin shutdown reports an error.
- Final native QUIC selection passed 140 cases / 2,375 assertions; the remaining
  P2P cases were not part of that selection. This includes the independent
  packet-count barrier regression (41 assertions). Synthetic packets are
  discarded before releasing the held drain, not presented as wire evidence.
- Seven affected suites passed three consecutive runs: structure, network-plugin
  structure, inventory, descriptor surfaces, plugins (224 cases), managed remote
  (10 cases) and DB Store (74 cases, including local MDBX/RocksDB coverage).
  The installed node consumer rebuilt and ran after the final native fix.
  Build parallelism remained `-j4`; full CI was not run.
- Both independent scoped source reviews closed their findings. Public node
  module count remains 13 and only the inventory surface digest changes; no
  production support state is promoted. Temporary task-owned package builds
  and the install prefix are removed after their processes finish; the active
  Stage 7 build and failure/success logs are retained.
- Next are private-network secret provisioning, DNS/dial/path and the remaining
  reachability, mDNS, relay, DHT/lifecycle and PubSub policies. Configured-path
  Go/Rust acceptance and whole-PR review remain open. This checkpoint is not
  Stage 7 completion or permission to merge the incomplete PR.

### Private-Network Secrets Checkpoint

- The optional `private-network` group maps a Secrets reference and the native
  Internet-egress policy. Startup reads purpose `p2p.private-network.psk`, requires
  exactly 32 decoded bytes and constructs the existing native pnet protector.
  No inline key, second decoder, network loop or new public module is added.
  Secrets retains source decoding and permission enforcement; the declarative
  configuration never stores the provisioned protector.
- The approved private profile requires direct TCP/Yamux, explicit direct-only
  paths and disabled relay client/service/discovery. Incompatible endpoints and
  explicit QUIC settings fail before Secrets access. Native preflight now also
  checks configured Rendezvous points, closing a late-failure gap even when
  discovery is disabled. Public QUIC Rendezvous remains valid. `deny-external`
  is an AutoNAT/external-protocol policy, not an IP firewall.
- Every Secrets result enters existing secure-erasing ownership before a stop
  check. A stopped setup does not initiate the next secret read or P2P adapter
  open. Shutdown waits for an admitted lookup, including when the shutdown
  caller is cancelled and another caller joins. Typed Secrets errors remain
  intact; temporary certificate/key strings keep their existing secure cleanup.
- Config/lifecycle coverage passed 29 cases / 203 assertions. Three additional
  runtime cases passed 135 assertions: real Secrets permissions and decoded key
  sizes; configured MDBX/RocksDB plus Noise/TLS/Yamux and binary exchange; and a
  held real-provider PSK lookup with unchanged ObjectDB operation counters after
  stop. The held-lookup test uses an instrumented memory driver, not a durable
  storage claim. The network tests use real configured disk backends with
  insecure mode disabled.
- Negative network tests independently witness server acceptance and failed
  handshakes. Wrong or absent PSK cannot increment successful authentication or
  session counters, and the same listener subsequently accepts the correct key.
  Checking only a thrown exception and an empty final session list was rejected
  as insufficient evidence. Fixture cleanup joins every owned operation even
  after an earlier join reports an error; frozen-field tests start from an
  otherwise valid accepted configuration.
- Native policy/private-network selection passed 21 cases / 217 assertions;
  the other P2P cases were not part of this selection. Both independent scoped
  source reviews closed their findings. This is not whole-Stage-7 approval.
- Seven affected suites passed three consecutive runs each: structure,
  network-plugin structure, inventory, descriptor surfaces, plugins (234 cases),
  managed remote (10 cases) and DB Store (77 cases, both local disk backends).
- The installed node consumer built and ran from a fresh dev-component install.
  Its completed build and install directories were removed, reclaiming about
  3.6 GiB. The active Stage 7 tree and failure/success logs remain. Builds used
  `-j4`; full CI was not run.
- These are local configured-path and native checks, not new configured-plugin
  Go/Rust evidence. No wire format, donor source, project version or production
  support state changed. Remaining work is DNS/dial/path, reachability, mDNS,
  relay, DHT/lifecycle and PubSub policy mapping, then configured Go/Rust
  acceptance and whole-PR review. PR #172 remains incomplete and unmerged.

### DNS, Dial And Path Checkpoint

- The canonical config maps sparse DNS resolver/expansion, dial staggering,
  UDP/IPv6 black-hole detector settings and `path.max-direct-endpoints` into
  existing native owners. Missing values preserve native defaults; unknown
  fields, bad sizes/durations/ports, scope errors and native cross-field limits
  fail before publishing a candidate. No new public module or plugin loop.
- Native path preflight now enforces the scheduler's existing `1..100` direct
  attempt ceiling. Both native protocol-open selection and plugin remote-API
  policy receive the configured value. Explicit connect still uses its own
  per-call budget; dial concurrency remains per logical dial, not node-wide.
- Seven configuration cases passed 915 assertions. Five additional configured
  runtime cases passed 240 assertions: recursive DNSADDR with foreign-peer
  filtering and authenticated Noise/TLS/QUIC binary exchange; contrasting result
  bounds 1/2 and actual post-resolution gater rejection; NXDOMAIN; cancellation
  and node shutdown during a witnessed no-reply query; and atomic rollback/freeze
  with observable DNS and diagnostics behavior. Both local MDBX and RocksDB
  drivers are exercised with real Secrets and insecure mode disabled.
- The shared test-only secure app fixture replaces the duplicated private-network
  setup rather than creating another runtime. Both existing private-network
  cases still pass their 128 assertions. The fixture wraps the official factory
  only to retain its owned plugin for direct configure tests; application-shell
  lifecycle guards are not mistaken for plugin freeze evidence. Cleanup joins
  both plugin shutdown and scheduler before reporting failures.
- The first runtime run failed on caller cancellation for both disk drivers:
  actual P2P `internal` (15), expected `canceled` (13). Native DNS cancellation
  already happened, but its Asio `operation_aborted` was swallowed by the
  expander's catch-all. The exact error now maps to `canceled`; A/AAAA/TXT
  regressions retain `internal` for unrelated system errors. Original failed
  logs and unchanged cancellation expectations are retained.
- The expanded native selection passed 139 cases / 3,029 assertions, including
  DNS expansion, scheduler cancellation/concurrency, public detector recovery,
  path management and policy bounds. The other native cases were not selected.
  Review corrected a runtime-test name: loopback does not prove public-network
  detector neutrality; that guarantee belongs to the separate native tests.
- Seven affected suites passed three consecutive runs: structure, network-plugin
  structure, inventory, descriptor surfaces, plugins (241 cases), managed remote
  (10 cases) and DB Store (82 cases). The installed node consumer built from a
  fresh dev-component install, including the corrected native archive, and checks
  the new config types and out-of-line schema definitions. Full CI was not run;
  coordinator builds remain `-j4`.
- Public module count stays 13; inventory changes only the public-surface digest.
  No wire, donor source, persisted schema, project version or support-state
  promotion. These local configured tests do not establish new Go/Rust evidence.
- Remaining owners are reachability, mDNS, relay, DHT/lifecycle (including raw
  bootstrap address mapping) and PubSub, then configured Go/Rust acceptance and
  whole-PR review. This checkpoint is not Stage 7 completion; PR #172 remains
  draft and must not be merged yet.

### Grouped Policies And Configured Interop Checkpoint

- The canonical config now groups identity, persistence, bootstrap, topology,
  Rendezvous, peer exchange, reachability, mDNS, relay and PubSub settings by
  their existing native owner. Sparse overrides preserve unspecified native
  defaults. Metadata stays after type declarations; schemas and mapping use
  coherent aspect units instead of more public config modules.
- DHT record capacities now reach the operational store and ObjectDB adapter.
  Native `node::options::dht_records` owns per-profile store options; the plugin
  supplies persistence without replacing configured bounds. Two configured
  runtime cases passed 120 assertions through MDBX and RocksDB, covering quotas,
  reopen and profile isolation. A separate preflight case passed 21 assertions
  without reading Secrets or opening the P2P adapter for an invalid policy.
- Three configured service cases passed 156 assertions for reachability role
  advertisement, private-profile restrictions, Ping/events and AutoRelay
  reservation, renewal, traffic and withdrawal. These are Forge-to-Forge
  runtime checks, not new donor evidence.
- PubSub maps version/fallback, signature/flood policy, wire bounds, validation
  retry, IDONTWANT, Partial Messages and scoring into the existing native
  engine. Typed pre-start PubSub options remain an authoritative complete
  snapshot; YAML does not overwrite application callbacks or explicit native
  options. Sixteen pure cases passed 630 assertions. Two configured runtime
  cases passed 68 assertions through both disk backends, including signed and
  strict-unsigned delivery, size rejection, typed precedence, rollback and
  startup freeze. The 1 MiB payload case is Forge-to-Forge only; donor RPC
  defaults differ and are not a negotiated universal message limit.
- Review found one native retry-duration gap: a representable milliseconds
  value can overflow the steady clock, and adding a valid duration near the
  clock maximum also needs saturation. Native validation now rejects unrepresentable
  retry and heartbeat durations; a shared private helper safely converts and
  saturates accepted deadlines at all affected call sites. Targeted native,
  runtime and typed-config regressions passed, including no early retry/tick,
  rollback before Secrets and bounded stop/join. Independent scoped review
  found no actionable findings in this fix.
- The new configured-plugin interop fixture owns an official application shell,
  Secrets, a real named MDBX/RocksDB ObjectDB store and the node plugin. It uses
  public plugin APIs, not a raw replacement node. Its first closed matrix has
  42 cases per backend for public TCP Noise/TLS/QUIC, DNS and private-network
  positive/negative controls. This is a transport/identity subset, not the full
  Stage 7 service acceptance matrix.
- The development MDBX matrix recorded 30 passes, 11 failures and one separately
  tracked Go QUIC close failure. Failed logs and receipts are retained. PSK
  rejection-counter expectations and a Rust connection-observation failure
  were investigated separately. Repeated immediate-disconnect Go QUIC trials recorded
  four passes and six canceled Forge stream closes. No failed close is relabeled
  successful because the payload arrived.
- The next complete development transport matrix recorded 81 passes and three
  QUIC echo-close failures out of 84, through MDBX and RocksDB. All private
  positive/negative controls passed. Go dial synchronization now preserves the
  actual stream through independently witnessed Forge close; negative PSK
  checks distinguish logical timeout from attributable exhausted pre-auth roots.
  Rust TCP completion evidence uses its actual successful close snapshot and
  never repairs missing facts from the final connection snapshot.
- A bounded opt-in Go QUIC listener subsequently proved actual request EOF
  before checked close. Eight predetermined public/DNS cases through both DB
  backends passed strict semantic and captured-source validation. This fixture
  change does not patch the donor library or hide the preceding RED results.
  Four predetermined Rust-to-Forge QUIC cases subsequently passed through both
  backends with a same-stream barrier, actual read/write hashes, authenticated
  connection observation and joined fixture tasks. The existing upstream Rust
  whole-host QUIC shutdown limitation remains separate. The subsequent complete
  development rerun passed all 84 cases, including strict semantic and captured-source
  validation through both backends. Independent scoped review of the Rust barrier and Python
  receipt/ownership checks found no actionable findings; this is not a whole-PR
  acceptance verdict.
- Six focused suites passed, and the installed node package consumer built and
  executed after updating its old flat config references to the thematic groups.
- The installed resolver consumer also built and executed. Both temporary
  package-consumer build directories were removed after checking that no process
  or file owner remained; the common install and active Stage 7 build were kept.
- Thematic review found inconsistent Rendezvous default TTL acceptance. Native
  validation now requires `min <= default <= max`; seven native boundary assertions
  and seven lifecycle/config cases with 1325 assertions passed, including rejection
  without replacing the previously accepted topology snapshot.
- The configured DHT value fixture now shares existing `/pk` and `/ipns` golden
  records with the raw fixture. Four development Forge-writer/Go-server/fresh-Forge-reader
  exchanges succeeded through MDBX/RocksDB after the writer exited. This is a
  smoke result, not the final bilateral Go/Rust matrix or durable restart proof.
  Its session-history capture has a 64-entry budget and fails on overflow rather
  than silently losing observations. The independent fixture review is source-clean;
  the subsequent 16-case development matrix passed in both Go/Rust roles through
  both disk backends, including final semantic and captured-source validation.
  Twenty-two case/evidence regressions and 25 shared canonical-acceptance tests
  passed. Negative source tests first establish a valid baseline, so unrelated
  macOS temporary-path aliases cannot manufacture rejection coverage.
  Complete service acceptance remains pending.
- The configured PubSub development matrix passed eight cases through both
  disk backends and Go/Rust. It proves signed bidirectional delivery, exact
  payload-limit acceptance/rejection and authoritative typed configuration
  retaining application callbacks. Fifty-four configured PubSub and shared
  acceptance Python tests passed. Fixture self-tests cover 55 checks.
  A GossipSub-only Rust peer is correctly recorded as authenticated with
  unsuccessful Identify, not made artificially identified. The base transport
  and DHT tests retain their independent Identify requirements. Earlier failed
  PubSub runs remain preserved; control-field collisions, duplicate JSON source
  keys and a redacted empty Secrets container were fixed rather than relaxing
  validators. Native scoring/extension and whole Stage 7 promotion are separate.
- Configured AutoRelay passed eight development Go/Rust cases through MDBX and
  RocksDB: native client acquisition, renewal, loss/replacement and address
  withdrawal, plus the official plugin as a relay service. The existing runner,
  donor observers and validators are composed, not copied into another runtime.
  Review strengthened the configured proof to require both Forge seed receipts
  and all three lifecycle echoes, bound to actual donor identities and carrier
  sessions with completed writes/closes. All eight retained live sources passed
  revalidation; 68 Python checks and 65 C++ fixture self-checks passed. The configured
  boundary proves joined owners, a retained diagnostics API rejecting after
  detach, and a retained host-event subscription ending. It does not fabricate
  raw-node snapshots before startup or after detach, and raw post-stop counter
  requirements stay unchanged.
- Four configured value/provider cases passed through both disk backends. A
  fresh independent reader discovers the remote provider through a server;
  local and total provider quotas, profile isolation, restart reconfirmation,
  withdrawal and retained remote records are checked without a self-query
  masquerading as network discovery. Shutdown/reopen is not crash recovery.
- Configured Rendezvous connects to a hidden peer without manual discovery,
  verifies its authenticated Identify result and Ping, and leaves a configured
  static-only control idle. After that inactivity check, the independent control
  uses read-only DISCOVER to verify both exact signed registrations. Joined
  seeker shutdown removes only its registration while the hidden peer remains;
  the native 7200-second TTL excludes natural expiry as an explanation. The
  MDBX/RocksDB runtime check passed. Receiver metrics alone are not used to
  identify a registration.
- Two explicit LAN tests passed through MDBX and RocksDB: private mDNS discovery
  without seed addresses plus disabled/withdrawal controls, and host-event
  coalescing after genuine Identify address confirmation and TTL expiry. The
  latter retains the authenticated session while the observation expires.
  `FORGE_TEST_P2P_LAN_ADDRESS` must select a real non-loopback IPv4 interface;
  absent input is reported as `NOT_RUN`/CTest skipped, not success. These are
  configured Forge-to-Forge runtime tests, not new Go/Rust multicast or public
  AutoNAT classifications. Independent review strengthened fresh host-event
  subscription assertions to compare all snapshot fields, not just generation;
  the subsequent two LAN tests passed again. The subsequent configured AutoNAT
  development run passed eight isolated Linux cases: both ObjectDB backends,
  Forge client/service, v1 with Rust and v2 with Go. Its explicit configured
  boundary reuses the existing process/network owners and preserves raw-node
  checks; 87 shared AutoNAT regressions and 24 configured regressions passed.
  One v1 observer leaves the native host quorum unknown, while v2 verifies the
  actual address-level state. The first incorrectly launched shared-namespace
  attempt was refused before spawning participants and is retained separately.
  A subsequent review tightened the C++ fixture to its closed isolated address
  set; no general-purpose routability classifier is duplicated in the fixture.
- These results use an uncommitted development tree, not exact-head acceptance.
  A subsequent independent review found four mDNS upper bounds checked only at
  service construction. They now live in the common native policy validator,
  without a second plugin-specific limit list. Exact/+1 enabled/disabled tests
  and rejected-candidate rollback passed: seven native cases and nine focused
  plugin cases. The bounded fix received independent source-clean review.
  The refreshed installed `net_p2p`, `api_p2p`, node and resolver consumers also
  built and executed successfully. Each temporary package build was removed
  after process completion and an open-file check; about 8 GiB of cumulative
  package intermediates were removed without touching the shared build or
  neighboring worktrees. Structure, network-plugin structure, descriptor and
  inventory gates passed; the focused evidence foundation passed 254 Python tests.
  Complete configured service exchanges, package checks, fixture provenance and
  independent final reviews remain required. PR 172 is still incomplete and
  unmerged; no support claim or project version changes at this checkpoint.
- The complete policy implementation was committed as `96665817`. Independent
  reviews found no remaining source findings in the production slice. Configured
  node (84 cases), DHT (16) and relay (8) matrices passed through both MDBX and
  RocksDB on macOS, with their separate provenance/evidence validators. These
  results are scoped to that commit, not a blanket final-head verdict.
- Configured PubSub exposed a fixture receipt-publication race: the native write
  had completed, but the periodic donor result snapshot had not yet recorded it.
  `a48332e7` waits for the exact matching native-write receipt within the existing
  case deadline, after actual Forge delivery. It adds no republish, network retry,
  donor patch or relaxed final assertion. Focused regressions cover delayed,
  missing and mismatched receipts; 186 configured evidence tests passed.
- On `a48332e7`, the Linux configured PubSub matrix passed all eight cases and its
  independent validator. Configured AutoNAT also completed all eight live cases,
  but the separate evidence validator rejected quiet donor stdout under the
  configured wrapper. Its native namespace process ownership was not classified
  by the shared index checker. Empty result JSON is still forbidden; resolving
  this classification gap requires positive and negative ownership regressions,
  not exempting configured proofs from indexing. Both successful network results
  and the failed validator logs are retained outside the source tree.
- Final acceptance must rerun all five configured matrices and their independent
  validators on one clean final commit, after fixture-lock validation and source
  review. The recovered Linux fixture includes both durable backends; no missing
  dependency or Docker interruption counts as PASS. Full CI, Stage 8 production
  proof and downstream product integration remain outside this PR.
- The quiet-stdout correction retains isolation only for registered raw roots
  and the exact configured AutoNAT `native` edge. Forged sibling/deep suite
  labels, conflicting process ownership, empty JSON, altered hash/size and
  incomplete index coverage are rejected. The corrected slice passed 105
  AutoNAT/shared regressions and all 194 configured Python regressions.
- Final-gate review found the earlier configured score-aware pruning obligation
  still open. Its new real plugin/TLS test passed 100 assertions: opposite tag
  assignments change the survivor, even after refreshing the lowest-scored
  connection. It checks exact peer/session identities and two removals with no
  hard protection. This fixture uses memory persistence; durable-backend parity
  remains a separate gate.
- A repeated macOS DB Store run exposed Rendezvous fixture attribution races
  and an unregister shutdown failure; the failed logs are retained. Hidden now
  needs only its configured point, preventing reverse autonomous dialing, and
  seeker waits for both authenticated Identify and published discovery provenance
  within the existing bound. This corrected fixture passed 100 assertions across
  Linux MDBX/RocksDB. It does not close the independent runtime gap between local
  UNREGISTER write completion and remote stream completion; that gap needs a
  bounded owned-completion fix and regressions before merge.
- The subsequent native candidate now retains the UNREGISTER stream/resource
  owner until typed normal remote EOF, after drained bytes, within the existing
  operation deadline. Remote EOF is stream completion, not a protocol ACK.
  Reset, cancellation and connection loss do not grant EOF; the additive EOF
  subtypes preserve existing closed-category catches and numeric codes, including
  the native QUIC-to-transport translation. No ACK, retry, sleep or global close
  redesign was added.
- The coordinator's Linux checkpoint passed six focused QUIC/Rendezvous cases
  (609 assertions) and two Yamux cases (10 assertions). The complete five CTest
  gates passed: structure, network-plugin structure, Yamux, native QUIC/P2P and
  feature inventory (`forge-stage7-eof-full-native.log`). Plugins, managed-remote
  and DB Store all passed (`forge-stage7-eof-plugin-regressions.log`), followed by
  five successful DB Store repeats (`forge-stage7-eof-db-repeat.log`). Descriptor
  validation, all 194 configured Python regressions and the shared checker
  self-test also passed. Logs are retained outside the source tree. These are
  dirty-candidate results, not acceptance evidence for a clean exact head.
- A negative mutation removing the remote-completion barrier failed the
  TCP/Yamux held-operation check as expected. The restored
  `libraries/net/p2p/node_impl_topology.cpp` SHA256 is
  `35332ec7d0488b25de5a2e75342ff0ce2c5232f20e0e115fabec6432fe11e854`.
  This mutation proves the TCP/Yamux regression gate only; no QUIC mutation
  result is claimed.
- The new Forge-to-Rust automatic-manager-unregister phase compiled. It
  preserves the original register/discover checks and adds a
  distinct managed identity, joined refresh/stop, full no-cookie observations,
  exact managed/control identities, signed control continuity and TTL7200,
  with logged unregister failures treated as failures even when stop returns.
- The subsequent focused Rust live run on source head
  `0834111f650b2a25d50dc4a80bdd98f86275d43e` recorded 2/4 PASS: both
  Forge-to-Rust `rendezvous_register_discover` and `rendezvous_lifecycle` failed,
  while both reverse Rust-to-Forge cases passed. The Forge failure was
  `peer_write_rejected` before native FIN submission (`stream.cpp:69`,
  `quic_engine_stream.cpp:185`); the failed artifacts are retained. This is a
  focused four-case run, not full canonical Stage 6 acceptance. Failure may
  precede the automatic-unregister phase, so that phase is not live-proven.
  Earlier native/local PASS results do not override this RED checkpoint.
- Donor review confirms Rust's one-request-per-substream path writes the
  request, half-closes writing and then reads the response. The next dirty
  candidate adds `async_close_write` before register/discover response reads
  and the existing unregister remote-EOF barrier. Callers serialize it with
  writes and terminal close; reading, cancellation and the exact stream
  resource reservation remain alive until terminal cleanup. QUIC and Yamux
  are the intended supported models; other transport models default to typed
  `transport::exceptions::unsupported_protocol`. QUIC half-close success must
  prove submitted/drained FIN, not a no-op terminal close after prior rejection
  or reset. Native review/regressions and a new live run remain pending; no fix
  PASS, swallowed reset, donor patch, retry or relaxed validator is claimed.
  Rust can stop its request receive side after decoding a frame without waiting
  for EOF, so a legitimate STOP_SENDING can still race FIN submission; code zero
  alone is not successful completion.
- The write-half-close regression checkpoint passed nine focused native cases
  and 899 assertions: drained FIN, pre-FIN rejection, independent read reset,
  late STOP, cancel-before-FIN and Rendezvous resource/error ownership. The
  late-STOP fixture holds the existing asynchronous cleanup owner, not an OS
  thread or owner strand. Restoring the rejected post-FIN STOP check made this
  test fail at its exact success assertion; restoring the correction returned
  `quic_engine_stream.cpp` to SHA256
  `31cf7233c2b34b56b6d0f46ce26268a7445e67e4962a0d15e155f7d93bc81772`.
  Full Yamux passed 20 consecutive runs after its error-worker observation
  barrier was fixed. Earlier failed native/test-barrier results remain retained;
  these dirty-candidate checks do not replace clean-head configured acceptance.
- These focused results do not complete Stage 7.
  The final configured matrices and independent validators, installed-package
  checks and clean exact-head acceptance remain pending. Rust's original
  whole-host QUIC shutdown remains `NOT_PROVEN`; these native/local results do
  not change that separate limitation or complete Stage 7.
