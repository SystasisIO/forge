# P2P Node Plugin

`forge::plugins::net::p2p::node` owns one shared `forge_net_p2p` node and exposes typed
contribution APIs for protocol handlers and API-over-P2P publication, consumer
PubSub, bounded read-only diagnostics and native host events.

## Current Support State

The plugin provides the Stage 5 persistence and topology configuration surface:
identity material is loaded through Crypto Secrets, peer/Rendezvous state and
per-profile DHT values/providers are persisted through separate adapters in
one dedicated DB Store Object layer, and hydration completes before listeners
open. The low-level node owns bootstrap, automatic Identify/Identify Push,
scoped network resources, multi-profile Kademlia routing, durable records and
provider maintenance. It also owns the managed/static mode selection,
watermarks and explicit Rendezvous/Forge Peer Exchange source configuration;
all timers, queries, dials and pruning remain owned by the low-level node. The
plugin does not contain a second network maintenance loop.

The approved Stage 7 consolidation moves the former standalone PubSub and
diagnostics plugins into node-owned roles. Consumer registrations and callback
dispatch live in one private PubSub component; diagnostics and events delegate
to the same native node. This source slice is not completion of Stage 7 or an
acceptance result. Compilation, migrated ownership/failure tests and the remaining
Stage 7 gates are tracked in
[`forge-p2p-plugin-integration-v1.md`](../../../../docs/iterations/forge-p2p-plugin-integration-v1.md).
No production or live donor interoperability claim is made by this README.

Insecure memory mode remains an explicit local-test path only. Current support
classifications live
in
[`p2p_feature_inventory.json`](../../../../tests/libp2p_interop/p2p_feature_inventory.json);
isolated codec and interop fixtures do not promote this plugin to production.

## When To Use

- A Forge application needs one shared P2P node managed by `forge_app`.
- Product plugins need to publish protocol handlers or typed APIs over the same
  peer/session substrate.
- Application plugins need topic publish/subscribe with async validation and
  bounded per-consumer callbacks over that shared node.
- Operators need bounded diagnostics or host-state subscriptions without private
  network internals. The separate resolver still composes over the same node.

## When Not To Use

- Do not create product routing, durable queue or authorization policy in this
  plugin.
- Do not instantiate raw `forge::net::p2p::node` separately inside application
  plugins.
- Do not enable insecure test mode outside local tests.
- Do not treat PubSub as a durable queue, replay log, product event store or an
  exactly-once delivery mechanism.
- Do not use diagnostics as a health-policy engine, alert router or readiness
  verdict, or expose unbounded state to remote callers.

## Identity

- Target: `forge_plugins_net_p2p_node`
- Package component: `plugins_net_p2p_node`
- Plugin id: `forge.plugins.net.p2p.node`
- Main API id: `forge.plugins.net.p2p.node`
- Plugin version: `8.0.0`
- Main API contract: `3.0`
- Extra API ids:
  - `forge.plugins.net.p2p.node.configuration` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.dht` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.protocol` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.pubsub` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.diagnostics` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.events` (contract `1.0`, local only)
- Config section: `plugins.net.p2p.node`
- Public modules:
  - `forge.plugins.net.p2p.node.plugin`
  - `forge.plugins.net.p2p.node.api`
  - `forge.plugins.net.p2p.node.dht_api`
  - `forge.plugins.net.p2p.node.configuration_api`
  - `forge.plugins.net.p2p.node.protocol_api`
  - `forge.plugins.net.p2p.node.protocol_registration`
  - `forge.plugins.net.p2p.node.pubsub_api`
  - `forge.plugins.net.p2p.node.diagnostics_api`
  - `forge.plugins.net.p2p.node.events_api`
  - `forge.plugins.net.p2p.node.peer_contribution`
  - `forge.plugins.net.p2p.node.config`
  - `forge.plugins.net.p2p.node.types`
  - `forge.plugins.net.p2p.node.exceptions`

`config` owns declarative settings, policy enums and their schema declarations;
schema definitions live out-of-line in `config.cpp`. `types` owns consumer DTOs
only and does not import configuration/schema. `exceptions` is the single plugin
error set and category. Role APIs and owning registration tokens keep their own
modules; no aggregate or forwarding modules are supplied.

## What It Provides Today

- Starts and stops a shared P2P node through the `forge_app` lifecycle.
- Acquires the configured physical store and registers private P2P-state
  ObjectDB models during `after_initialize()`.
- Loads certificate/private-key secrets, validates or explicitly resets the
  private cache schema, opens ObjectDB persistence and performs bounded
  hydration during `startup()` before opening any listener.
- Maps config into listen/bootstrap/advertised endpoints and relay/path policy.
- Maps topology watermarks, configured Rendezvous points/namespaces and Forge
  Peer Exchange policy into the node-owned topology manager.
- Passes bootstrap policy to the node, which owns bounded startup, reconnect
  backoff and connection-manager protection.
- Lets application plugins publish typed APIs over a P2P protocol id and retain
  an owned publication handle for explicit close/drain.
- Opens typed remote API handles to peers through `remote<Interface>()`.
- Delegates ordinary connect and Ping to the same node, with native options and
  cancellation, and owns bounded protection/score contributions per consumer.
- Delegates profile-aware peer/provider discovery, owned provider publication,
  validated value operations and typed IPNS creation through `dht_api`.
- Publishes raw bytes and serializable typed values, with per-consumer Full and
  Partial subscriptions, validation, quotas and owned callback cleanup.
- Exposes bounded native network/resource/PubSub snapshots, peer filtering and
  lookup, per-profile DHT routing status and native PubSub peer scores.
- Delegates native host-state snapshots and subscriptions through `events_api`.
  No public source contract, node accessor or second API-provider hop is involved.

### Pre-Startup Contributions

Import `forge.plugins.net.p2p.node.configuration_api` for local composition. The
API accepts one nonnull native `connection_gater` and one complete
`std::vector<forge::net::p2p::dht::value_policy>` per configured custom,
values-enabled DHT profile. Supply application-owned `validate`, `select` and
`expiry` callbacks with disjoint native key prefixes. Unknown profiles,
values-disabled profiles and Amino overrides are rejected; duplicate
contributions and changes after startup or stop are typed conflicts.

`set_pubsub_options(forge::net::p2p::pubsub::options)` supplies typed native
PubSub policy, including application scoring and tracer callbacks. It requires
explicit `pubsub.enabled: true`; it does not turn on native support implicitly.
Options use the same immutable candidate preparation, validation and commit/freeze
path. Pre-start YAML updates retain a supplied native contribution; they cannot
disable PubSub and silently discard its callbacks. Consumer topic policies and
quotas remain declarative, while supplied native options replace the native
defaults derived from that declaration. Native bounds and capability errors remain
authoritative: for example, Partial needs both declarative `partial-messages`
admission and appropriate native Partial/version options. This is not a live
native-options setter. Gater and DHT duplicate rejection is unchanged.
Repeated pre-start `set_pubsub_options` replaces the previous native contribution
through the same snapshot compare-and-swap commit; retired callback captures are
destroyed outside the configuration mutex.

Configuration is an immutable policy snapshot, not a native node or a resource
store. A custom profile declaration may await its C++ contribution, but cannot
be passed incomplete to the runtime. Startup completes and validates all native
options before its first await, Secrets reads or P2P persistence transactions.
Native `validate_declaration(profile)` checks declaration policy only;
`validate(profile)` still requires every value callback. Neither executes
application callbacks. No permissive placeholder policies are installed.

After startup, only diagnostics bounds may change through the existing
`plugin::configure` lifecycle entry point. The complete configuration must still
decode and validate, and its frozen node/PubSub declarative portion must be
structurally unchanged. A mixed or invalid update fails atomically; it cannot
rebuild the node, overwrite typed contributions or change network policy.
Configuration after stop is rejected. The read-only diagnostics API has no setter.

Candidate preparation, callback copies and destruction occur outside the
configuration mutex. Commit compares the previous snapshot and admission again;
a nested or concurrent update is preserved and the stale update fails. Existing
protocol tokens and PubSub contributions survive an unrelated policy update.
Gater callbacks must be nonblocking and thread-safe; callback captures must own
their consumer state rather than borrow a destroyed application object.

`initialize()` binds available DB Store and Secrets providers exactly once.
Later pre-start changes cannot introduce an unbound dependency, but references
to an already bound Secrets provider may change or be cleared. `after_initialize()`
fixes the exact Store name, including empty, before awaiting its handle; policy
updates remain allowed. Production startup requires the initialized Store.
The explicit empty-store test path can start after `initialize()` alone.
No PEM or opened persistence handle is stored in the policy snapshot.

`request_stop()` closes configuration admission before requesting native stop.
`shutdown()` joins admitted startup and Store setup before releasing providers
or adapters, including an outstanding Secrets read. This does not add a second
runtime or claim that the earlier shared DB driver setup was resource-free.

For consumer PubSub, stop closes admission and signals owned Partial operations.
Awaited shutdown joins admitted operations, callbacks and callback-capture
destruction, then performs local native topic cleanup before native `async_stop()`
and persistence/provider release. Native full-topic unsubscribe remains usable
after `request_stop()`; cleanup does not require running-node admission.

### DHT Consumer Contract

`forge.plugins.net.p2p.node.dht_api` is a local application contract, not a new
remote protocol. Its `find_peer`, `provide`, `find_providers`, `put_value` and
`get_value` operations take the configured native `protocol_id` and native
query options. Unknown profiles, disabled profile capabilities, limits and
record validation retain the node's typed errors. No records or routing state
are duplicated in the adapter.

`provide` returns the native move-only `provider_registration`. Keep it alive
while serving the content. Its `async_withdraw()` releases local ownership;
previously announced remote records retain their independent TTL. IPNS creation
uses the configured host identity without exposing its private key.

An operation captures adapter ownership when its awaitable is created and
checks admission when execution starts. Calls before startup are rejected;
new calls after `request_stop()` are canceled. The node is retained throughout
an admitted operation, including its native cancellation and cleanup.

`request_stop()` synchronously closes managed-topology, Forge Peer Exchange and
new-session admission, then requests cancellation of bootstrap and maintenance
work. It intentionally does not tear down established sessions. `shutdown()` is
the completion barrier: it awaits topology workers, including best-effort
Rendezvous unregister, then tears down sessions and flushes/closes peer state
before releasing DB and Secrets handles. Application lifecycle code calls
`request_stop()` to initiate shutdown and awaits `shutdown()` for cleanup.
If peer-state close fails, the stopped node and its persistence ownership are
retained; the plugin never drops a persistence backend that still reports pending
close work. A subsequent `shutdown()` retries unfinished native/persistence or
protocol-registry cleanup. Concurrent callers share one attempt and its result;
a later retry cannot replace the result observed by its earlier waiters.
PubSub registration-component cleanup errors remain sticky and must not be
reported as successful disposal, but do not prevent the remaining cleanup.
An empty active session set is not treated as proof that STCP/Yamux cleanup has
completed.

The plugin does not implement product routing policy or durable application
queues. Core libp2p-style mechanics belong to `forge_net_p2p`; this plugin composes
that substrate for applications.

### PubSub Consumer Contract

Import `forge.plugins.net.p2p.node.pubsub_api` and request the local contract
`forge.plugins.net.p2p.node.pubsub` major 1. The role supports raw-byte and typed
`publish`, async validation `subscribe`, `unsubscribe`, per-consumer
`subscriptions()` and a `pubsub_snapshot` containing consumer counters and the
native core snapshot. `publish_options::sign` overrides the configured publish
default; a positive `subscribe_options::handler_deadline` overrides the configured
handler deadline. Typed publish serializes immediately, so its argument need not
survive until the returned awaitable starts.

`pubsub.enabled` defaults to false. Providing the role does not request native
PubSub support. With no explicit opt-in, operations reject with native
`unsupported`; before consumer initialization they reject with
`exceptions::plugin_not_initialized`. Stop closes new admission with native
`canceled`. Topic policy, missing registrations, capacity and message-size errors
remain typed: `topic_not_allowed`, `subscription_not_found`, `handler_limit` and
`message_too_large` live in `exceptions`.

Each topic serializes its native join/leave transitions independently. Pending
subscriptions reserve topic and handler capacity before waiting. A failed last
leave retains its subscription token and cleanup state: retry unsubscribe with
that token before joining again. Handler results aggregate terminal `accept`,
`reject` and `ignore`, or transient `retry`. Deadlines, handler failure and local
active-handler backpressure produce bounded retries, not product authorization.
Deadlines are cooperative: cancellation never detaches a handler that ignores it.

`message.source` is the immediate peer at the current gossip hop. `message.author`
is present only for a verified signed message; unsigned wire `from` values are not
exposed as verified authors. Use the immediate source for peer quotas and a
present author for signed-origin attribution. Signing is integrity support, not
business trust or an exactly-once delivery guarantee.

Async calls retain the registration component at call time and hold the same
native node throughout an admitted operation. Native calls and destruction of
consumer captures happen outside the component mutex. A handler may unsubscribe
itself; unsubscribe does not wait for that handler to return. Do not await plugin
shutdown from inside one of its own handlers. There is no hard shutdown deadline,
detached user code or background cleanup worker.

### Partial Registrations

`enable_partial(topic, full_fallback, partial_options, subscribe_options)` requires
receive and gossip callbacks and returns the native `partial_topic`. One Partial
owner is allowed per topic. The required Full fallback participates in ordinary
validation aggregation and reserves a handler slot while enable is pending; it
is neither returned by `subscriptions()` nor removable with an ordinary token.

`advertise_partial`, `forget_partial`, `partial_peers` and `send_partial` retain
native registration identity and bounds. A peer snapshot is not lasting send
permission: native send rechecks capability. Applications own group encoding,
reconstruction and revision rules. An admitted send does not hold the topic
transition gate during native I/O; caller, registration and plugin stop are
composed before sending, so downgrade can cancel it without waiting behind it.
Awaited shutdown still joins that send.

`disable_partial(token)` preserves ordinary handlers, and unsubscribing the last
ordinary handler preserves a current Partial owner. Failed native enable may
already have mutated state: compensation restores the Full-handler mux when
ordinary owners remain instead of removing their topic. Errors after a possibly
committed downgrade never reopen callback admission. The same token can retry
retained cleanup; a completed disable makes it stale.

Partial callbacks receive a scoped stop token composed from native, registration
and plugin stop. Stop reaches active retired callbacks before waiting for them.
Disable requests stop but does not wait for its own callback; shutdown joins
admitted callbacks and capture destruction. Keep stop callbacks nonblocking.
Native wrappers hold weak references to consumer state, not a second queue or
network loop. Concurrent component shutdowns observe one completion/error;
cleanup failures remain retained and rethrown.

Receive-side donor mapping: pinned Go pubsub `0ed6f6fd`,
`pubsub.go:1390-1430`, replaces per-topic state on each subscribed update and
decodes absent flags as false. Rust `22fb4c78`, `behaviour.rs:2134` and
`extensions/partial_messages.rs:158`, updates the Partial subscription before
duplicate subscription filtering. A Full-only resubscription therefore needs no
PRUNE. These are source mappings, not live plugin/donor interoperability results.

### Diagnostics And Events

Import `forge.plugins.net.p2p.node.diagnostics_api` and request
`forge.plugins.net.p2p.node.diagnostics` major 1. `snapshot()` uses configured
diagnostics bounds; `snapshot(native_options)` uses the explicit options without
replacing the configured defaults. `network()`, `resources()`, `pubsub()` and
`peers(peer_filter)` use bounded snapshots. `peer(id)` throws
`exceptions::not_found` when the bounded snapshot has no matching
peer. `only_connected`, `only_protected` and `limit` are local peer filters.

`routing_status(profile)` and `pubsub_scores()` return native `dht::routing_status`
and `pubsub::score_snapshot` values. Unknown DHT profiles retain native
`unsupported_protocol`. Reads before native construction or after detach reject
with `exceptions::plugin_not_initialized`; reads remain admitted after
`request_stop()` until detach. A read copies the native owner and, for a default
snapshot, its configured options before releasing the configuration mutex and
delegating. A concurrent diagnostics-only update cannot alter that read's bounds.
Returned value snapshots remain usable after plugin shutdown. There is no
adapter-owned routing/score cache, event queue, HTTP endpoint or logging sink.

Import `forge.plugins.net.p2p.node.events_api` and request
`forge.plugins.net.p2p.node.events` major 1 for `reachability_status()` and
`host_events()`. New event operations require a running node and are rejected
after stop. Issued native subscriptions outlive the API handle but end when the
node stops. The initial value is a complete native snapshot; later changes may
coalesce. Preserve `generation` and `resync_required` rather than treating values
as an unbounded event log. Each subscription allows one pending reader; canceling
a read does not close it, and `close()` closes that subscription only.

Diagnostics are local-only reads. Remote publication and authorization are a
product decision. Bounds limit peer/session/endpoint/protocol/relay snapshots;
snapshots must not reveal private key material or raw secrets, and they are not
an authoritative product-readiness verdict.

## Config

```yaml
plugins:
   net:
     p2p:
        node:
           listen: ["/ip4/0.0.0.0/udp/9443/quic-v1"]
           bootstrap: ["/dns4/bootstrap.example/udp/9443/quic-v1/p2p/<peer-id>"]
           bootstrap-requirement: allow-disconnected
           bootstrap-startup-budget-ms: 10000
           bootstrap-connect-timeout-ms: 2000
           bootstrap-max-parallel: 4
           advertised-endpoints: ["/dns4/node.example/udp/9443/quic-v1"]
           peer-id: ""
           peer-store:
              store: "p2p-peer-state"
           identity:
              certificate-secret: "p2p/node-certificate"
              private-key-secret: "p2p/node-private-key"
           api-codec: forge.raw
           api:
              deadline-ms: 5000
              max-frame-size: 16777216
           max-inflight-per-peer: 64
           max-sessions: 1024
           max-protocol-handlers: 1024
           allow-insecure-test-mode: false
           pubsub:
              enabled: false
              max-topics: 1024
              max-handlers-per-topic: 64
              max-active-handlers: 4096
              max-message-size: 1048576
              handler-deadline-ms: 5000
              allowed-topics: []
              denied-topics: []
              sign-publishes: true
              partial-messages: false
           diagnostics:
              max-peers: 1024
              max-sessions: 1024
              max-endpoints-per-peer: 64
              max-protocols-per-peer: 128
              max-relay-reservations-per-peer: 64
           topology:
              mode: managed
              peers:
                 low: 128
                 target: 160
                 high: 192
              refresh-interval-ms: 600000
              query-timeout-ms: 10000
              max-candidates: 256
              max-parallel-queries: 10
              max-parallel-dials: 4
              retry-jitter: 0.20
           rendezvous:
              role: client
              max-points: 4
              points:
                 - endpoint: "/dns4/rendezvous.example/udp/9443/quic-v1/p2p/<peer-id>"
                   namespaces: ["forge.content"]
           peer-exchange:
              enabled: true
              max-peers: 4
           path:
              policy: direct-preferred
           relay:
              trust: known-only
              client-enabled: true
              server-enabled: false
              public-allowed: false
              reservation-ttl-ms: 60000
              max-candidates: 4
```

`max-sessions` is the active-session watermark owned by the connection manager. The
plugin reserves one additional provisional connection in each system connection
dimension (total, inbound, and outbound), so a completed handshake can be evaluated
and can replace an eligible session at the watermark. For limits above one, the
derived prune watermark is one below `max-sessions`; the active-session count never
exceeds `max-sessions`.

Set `pubsub.enabled: true` explicitly before startup to use the PubSub role.
`partial-messages: true` selects native preferred GossipSub v1.3 and enables its
global Partial extension when native options are derived from the declaration.
False preserves the v1.1/disabled defaults. A typed native-options contribution
controls its own native version/Partial/scoring policy, while declarative
consumer admission and quotas still apply. Diagnostics fields default as shown;
each configured bound is positive and limited to 1,000,000.

Including `/p2p/<peer-id>` in each bootstrap endpoint pins the expected
authenticated peer and is recommended for production. Existing peer-less
bootstrap endpoints remain supported; their peer is learned only after the
authenticated connection succeeds.

The named DB Store must provide an Object layer dedicated to P2P peer and DHT
record state.
One authoritative schema marker versions the complete private row family, so
startup validates the format without scanning durable history. A missing marker
in nonempty storage or a version mismatch fails startup. With
`schema-policy: reset`, the v3 recovery path atomically removes the complete private P2P row
family, including peer, Rendezvous, DHT value/provider and sequence records.
Normal node startup hydrates peer candidates from configured bootstrap peers;
it does not recover DHT values, local provider ownership or Rendezvous
registrations. Products must publish or register those records again.
Secret policies must allow
`p2p.identity.certificate` and `p2p.identity.private-key` respectively.
`allow-insecure-test-mode` is for local tests only. Programmatic low-level node
construction may still provide identity material directly.

Plugin 2.0 removes inline certificate and private-key PEM configuration. Migrate
each value into Crypto Secrets and replace it with `certificate-secret` or
`private-key-secret`. This is an intentional plugin configuration break; the
low-level `forge_net_p2p` identity options remain source-compatible.

Plugin 3.0 replaces the single DHT capability/configuration surface with
explicit `dht.profiles` and adds `peer-store.schema-policy` for the recoverable
private cache. The local plugin API contract remains `1.0`; only Preview
configuration and low-level DHT source contracts change.

Plugin 4.0 adds the managed topology configuration shown above. Rendezvous
client activity is limited to explicitly configured points and namespaces;
Forge Peer Exchange is a Forge extension and is queried only on identified
peers that advertise its exact protocol. Set `topology.mode: static-only` to
disable autonomous discovery, dialing and pruning. The local plugin API
contract at that version remains `1.0`.

Plugin 5.0 upgrades the local node API contract to `2.0`. `publish_api(...)`
returns a move-only `forge::api::p2p::publication`: destroying or closing it
stops new API-session admission for that protocol and `async_close()` waits for
its in-flight API sessions. Publishing the same API protocol replaces its
generation behind one stable node handler; an older handle cannot unregister
the replacement. The historical unowned raw `publish_protocol(...)` entry point
is removed by the Stage 7 migration below.

The unreleased Plugin 7.0 added native Partial registration operations through
the former PubSub source and upgraded the main node API to 3.0 for owned raw
protocols. Plugin 8.0 consolidates those consumers as described below; the old
source contracts are not a supported composition path.

### Sparse Resource Limits

`resources` maps the existing native scopes: `system`, `transient`, `peer`,
`protocol`, `service`, `protocol-peer`, `service-peer`, `connection` and `stream`.
Each scope accepts `max-memory` (bytes), `max-file-descriptors`,
`max-inbound-connections`, `max-outbound-connections`, `max-connections`,
`max-inbound-streams`, `max-outbound-streams` and `max-streams`.

Missing or null individual budgets preserve that scope's native defaults.
An empty scope also preserves defaults; a null scope is invalid. Explicit zero
denies that resource, rather than selecting a default. Application order is native
defaults, existing session-derived defaults, then explicit resource patches.
Thus `max-sessions` never overwrites an explicit resource budget.

```yaml
plugins:
  net:
    p2p:
      node:
        resources:
          system:
            max-memory: 67108864
            max-outbound-connections: 32
          peer:
            max-streams: 64
          max-dial-attempts: 32
          max-dial-attempts-per-peer: 4
```

The four resource counters `max-dial-attempts`, `max-dial-attempts-per-peer`,
`max-relay-reservations` and `max-malformed-messages-per-peer` must be positive
when supplied. Negative, fractional, overflowing and unknown fields are rejected
before mutation. Only the exact application-shell-owned `enabled` selector is
excluded from node's unknown-field check. Limits freeze with native policy at
startup; rejected mixed updates do not partially change diagnostics bounds.

### Connections, Identify And Peer State

These groups are sparse patches in the same `node.config` module. Missing or
null individual settings preserve native defaults; empty groups are allowed,
but null groups and unknown fields are rejected. Counts and durations are
converted with range checks before publishing a new configuration snapshot.

`connections` configures `session-low-watermark`, `session-grace-period-ms`,
`session-prune-silence-ms`, and `dial-backoff-base-ms`, `dial-backoff-step-ms`,
`dial-backoff-max-ms`. The low watermark defaults to the existing plugin rule:
one for `max-sessions: 1`, otherwise `max-sessions - 1`. An explicit watermark
must be positive and no larger than `max-sessions`. Grace may be zero; silence
and backoff durations must be positive, with base no larger than maximum.
Resource admission budgets remain independent; protected sessions may prevent
pruning and cause a new connection to be rejected rather than evicting an owner.

`identify` configures `timeout-ms`, `max-message-size`, `max-total-message-size`,
`max-own-message-size`, `max-message-parts`, `max-protocols`,
`max-listen-endpoints`, `max-protocol-size`, `max-version-size`,
`max-public-key-size`, `max-signed-peer-record-size` and `max-push-operations`.
All must be positive and satisfy the native aggregate/message relationships.
Optional `protocol-version` and `agent-version` change advertised strings, not
authenticated identity. They do not relax peer-ID or signed-record checks.

`peer-state` configures `max-peers`, `max-rendezvous`, `max-pending`,
`max-endpoints-per-peer`, `max-protocols-per-peer`,
`max-relay-reservations-per-peer`, `max-relay-endpoints-per-reservation`,
`max-peer-record-bytes`, `hydration-page-limit`, `prune-page-limit` and
`max-persistence-waiters`. All are positive. These bound native operational
state and persistence work; they do not expose the peer store or change its
private persisted schema. `peer-store.store` still selects the dedicated DB
Store dependency.

```yaml
plugins:
  net:
    p2p:
      node:
        connections:
          session-grace-period-ms: 30000
          dial-backoff-max-ms: 120000
        identify:
          timeout-ms: 5000
          agent-version: "example-node/1.0"
        peer-state:
          max-peers: 2048
          hydration-page-limit: 128
```

All three groups freeze at startup. Failed validation or a mixed post-start
update preserves the old native and diagnostics settings atomically. Connection
maintenance, Identify exchange and peer persistence still run only in the node.

### TCP Security And QUIC Limits

`tcp.security` selects `tls-and-noise` (native default), `tls` or `noise` for
TCP/Yamux negotiation. It does not change QUIC's built-in TLS authentication.
TLS-only TCP requires `relay.client-enabled: false`: the current native relay
client needs Noise for its inner stream. Invalid combinations fail before
listeners open, rather than silently enabling another security protocol.

The optional `quic` fields overlay the existing native QUIC limits:

| Field | Actual enforcement |
| --- | --- |
| `max-connections` | Per-listener connections, including listener-backed pending dials; not a node-wide outbound limit |
| `max-streams-per-connection` | Active-stream check on local opens, plus advertised incoming bidirectional stream credit; not a combined inbound/outbound ceiling |
| `max-queued-bytes` | Per-connection outbound data retained until acknowledgement or terminal cleanup |
| `max-inbound-queued-bytes` | Per-connection queued UDP packet bytes, not application receive-buffer size |
| `max-inbound-queued-packets` | Per-connection queued UDP packet count |

Omitted fields preserve native defaults. Explicit zero is invalid; stream credit
cannot exceed QUIC's `2^60` wire limit. Unidirectional stream credit remains a
separate native setting, not part of this field. These are
QUIC settings, not Yamux or private TCP budgets. Node-wide limits remain under
`resources` and `max-sessions`. There is deliberately no `quic.max-frame-size`:
P2P uses native byte streams, not the separate opt-in QUIC framed-stream API.
Use the owning application protocol's message bounds instead; the Forge API
binding has its independent `api.max-frame-size`.

```yaml
plugins:
  net:
    p2p:
      node:
        tcp:
          security: tls-and-noise
        quic:
          max-streams-per-connection: 128
          max-queued-bytes: 8388608
```

Both groups freeze at startup. A rejected update also leaves unrelated
diagnostics settings unchanged. Mapping these policies does not by itself
prove configured-path Go/Rust interoperability.

### Plugin 8.0 Consolidation Migration

The maintainer approved this pre-stabilization source/package/configuration break.
It does not make stable APIs experimental or imply a wire/storage-format change.
Native protocol negotiation, validation, deadlines and persisted-cache formats
are unchanged by role consolidation. No compatibility aliases are provided.

- Register only the node descriptor for these roles; remove standalone PubSub
  and diagnostics descriptors, plugin IDs and package components.
- Replace `forge.plugins.net.p2p.pubsub.api` / `pubsub::api` with
  `forge.plugins.net.p2p.node.pubsub_api` / `node::pubsub_api`, requesting
  `forge.plugins.net.p2p.node.pubsub` contract 1.0.
- Replace diagnostics and events module imports with `node.diagnostics_api` and
  `node.events_api`, requesting the corresponding `forge.plugins.net.p2p.node`
  role IDs at 1.0. Import `node.types` for all consumer records, `node.config`
  for declarative settings and policy enums, and `node.exceptions` for errors.
  The consumer snapshot is named `node::pubsub_snapshot`.
- Replace former `pubsub_exceptions` / `diagnostics_exceptions` catch types with
  `node::exceptions::<same-name>`. Their category becomes
  `forge.plugins.net.p2p.node`; any code/category comparisons must migrate too.
  Node's existing `plugin_not_initialized=1`, `route_conflict=2` and
  `invalid_config=3` remain unchanged. Consolidated codes are
  `topic_not_allowed=4`, `subscription_not_found=5`, `handler_limit=6`,
  `message_too_large=7` and `not_found=8`. Native P2P error categories/codes,
  including cancellation and backpressure, remain native and unchanged.
- Move config from `plugins.net.p2p.pubsub` and `plugins.net.p2p.diagnostics`
  underneath `plugins.net.p2p.node.pubsub` and `.diagnostics`, and explicitly
  enable PubSub when needed. Old standalone sections, including `enabled: false`,
  and their `plugins.p2p.*` predecessors are rejected rather than ignored.
- Remove requests for `pubsub_source`, `diagnostics_source` and
  `host_event_source`. Application native PubSub policy contributions use
  pre-start `configuration_api::set_pubsub_options`, not a source adapter or a
  node accessor. The main node contract stays 3.0; configuration, DHT and raw
  protocol contracts stay 1.0. Resolver identities are unchanged.

Private `pubsub_service` owns actual registrations, callback dispatch and
per-topic transitions. Its immutable `pubsub_operations` dependency delegates
native mutation calls in production; deterministic component tests can replace
those calls with explicit barriers and before/after-mutation faults. The same
private `interface()` factory constructs the production role implementation.
There is no exported source substitute, test hook or second provider registry.
This component evidence checks local ownership/repair, not wire behavior;
authenticated native-network and installed-consumer gates remain separate.

### Owned Raw Protocols

Import `forge.plugins.net.p2p.node.protocol_api` and
`forge.plugins.net.p2p.node.protocol_registration`; request the local contract
`forge.plugins.net.p2p.node.protocol` major 1, revision 0. Migrate
`publish_protocol(id, handler)` to `register_protocol(id, handler)` and retain
the returned move-only registration. Duplicate names fail. To replace a raw
handler, call `replace_protocol(current_token, id, handler)`; foreign, closed
or superseded tokens fail. Closing an old token cannot remove its replacement.

Pre-start and live registrations share one bounded registry with API publications.
Built-ins (including currently disabled services) and every configured DHT
profile name, including client profiles, are reserved. Active, attaching and
failed-closing names consume `max-protocol-handlers` capacity.

`close() noexcept` and token destruction close admission, not streams already
admitted to that generation. They do not promise callback/stream drain. Capture
destruction happens outside registry/configuration locks. API publication
cancellation and `async_close()` session drain remain distinct and unchanged.
If native register/unregister throws after possibly changing the handler,
admission stays closed and the name remains claimed until a retry unregister
returns normally; `false` confirms absence. Repeated raw close, a subsequent
registration, or shutdown performs the retry under the same claim; there is no
detached retry worker. An API publication's already failed drain remains sticky;
retrying registry cleanup does not rewrite that result. A still-failing shutdown
reports the error.

`protocol_api::open(peer, id, native_open_options)` retains the same plugin/node
owner before suspension and delegates the existing native open and cancellation
contract. It exposes neither the node nor its secrets. New I/O fails before
startup and once plugin stop admission closes. This local API does not change
wire negotiation, native transport policy or deadlines.

Donor boundary: pinned Go `p2p/host/basic/basic_host.go` implements
`RemoveStreamHandler` by removing mux admission and emitting a protocol update,
not by closing already admitted streams; `NewStream` retains normal dialing,
negotiation and context cancellation. Forge adds local generation/name ownership,
not a new wire protocol. These source mappings are not live donor acceptance.

Private `protocol_operations` delegates install/remove to the same native node.
Registry unit tests replace only those two calls with a map, explicit before/after
mutation faults and held-attach barriers. They check stop/close races and uncertain
cleanup ownership; separate native TCP tests check admission and retained streams.
The fault tests do not claim to provoke an actual Identify Push allocation failure.

Consumers of the main node contract must request major 3; custom implementations
remove the old raw publication method and implement connect, ping and the peer
contribution methods below. Resolver contract identities are unchanged.

### Connect, Ping And Peer Contributions

`api::connect(endpoint, node::connect_options)`, its raw `multiaddr` overload,
and `api::ping(peer, node::open_options)` use the existing native executor,
identity checks, deadlines and cancellation. Awaitables retain the implementation
when created and retain the same node during execution. Admission is checked on
execution: before startup it fails with `plugin_not_initialized`; after
`request_stop()` it fails with native `canceled`. These methods do not add a
second network runtime, fallback policy or protocol.

`protect_peer(peer)` and `tag_peer(peer, value)` return a move-only
`peer_contribution`. Keep each token for the desired ownership duration;
`close() noexcept`, destruction and plugin stop remove only that contribution.
`is_peer_protected(peer)` queries actual native protection, including other
owners. Consumers cannot select global native keys, remove bootstrap/relay
protection, or replace another consumer's score. Closing protection does not
close the connection and makes no remote acknowledgement or drain promise.

Both kinds share bounded per-peer and tracked-peer capacity from the native
connection-manager topology limits (currently 16 and 1024 respectively, with a
128-byte native key bound). Native tag capacity also remains authoritative.
Positive and negative score subtotals are checked separately, including
`INT64_MIN`, so removing these contributions in any order cannot overflow.
Allocation and reservation precede native mutation; typed native rejection
rolls the reservation back. Cleanup moves preowned peer/key arguments and
performs allocation-free keyed native removal before releasing quota.

Stop closes admission and explicitly removes owned keys: native connection
manager reset intentionally retains tags/protection. Late handles cannot admit
new work; outstanding tokens become inactive. Facade destruction requests stop
but does not replace the required awaited application shutdown.

Focused component tests use the real native tag/protection registry to check
delegation, rollback, mixed-sign extremes and preservation of other owners.
Plugin TCP tests check authenticated sessions, Ping and bounded canceled dials.
Configured pruning is covered by a real TLS/Identify plugin regression: with
zero grace and a low watermark of one, admitting another peer prunes two
unprotected sessions while preserving the protected peer. The fixture checks
session identities and the native pruning counter, not just token activity.
It uses explicit memory persistence and is not configured Go/Rust evidence.

Plugin 6.0 upgraded `forge.plugins.net.p2p.node.diagnostics_source` to contract
`2.0`: diagnostics endpoint records now carry raw `multiaddr` values in their
`address` field so DNS address carriers survive. No v1 alias is provided.
The earlier Stage 7 diagnostics-source revision added `routing_status(profile)`
and `pubsub_scores()` alongside `snapshot(options)`. Plugin 8.0 removes that
source contract and the standalone facade; use the node-owned diagnostics role
described above. Native return types and typed unknown-profile errors remain.
The private ObjectDB P2P cache marker is v3;
v2 caches must use the existing recoverable `schema-policy: reset` path rather
than being hydrated.

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_net_p2p`
- `forge_api_transport`
- `forge_config_core`
- `forge_schema`
- `forge_raw` (public dependency for typed PubSub helpers)
- `forge_plugins_db_store`
- `forge_plugins_crypto_secrets`

## Examples

### Publish A Typed API

```cpp
import forge.api.core.binding;
import forge.api.p2p.publication;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;

class catalog_p2p_plugin final : public forge::app::plugin {
 public:
   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto p2p = context.apis().get<forge::plugins::net::p2p::node::api>(
         {.id = {"forge.plugins.net.p2p.node"}, .major = 3});

      auto plan = forge::api::core::binding()
         .serve(context.apis())
         .export_api<catalog_api>()
         .build();

      publication_ = p2p->publish_api(
         std::move(plan), forge::net::p2p::protocol_id{.value = "/catalog/api/1"});
      co_return;
   }

 private:
   forge::api::p2p::publication publication_;
};
```

```cpp
registry.register_plugin(forge::plugins::db::store::plugin::descriptor());
registry.register_plugin(forge::plugins::crypto::secrets::plugin::descriptor());
registry.register_plugin(forge::plugins::net::p2p::node::plugin::descriptor());
```

### Subscribe And Publish

Enable `plugins.net.p2p.node.pubsub.enabled` before startup. No separate PubSub
descriptor or source API is required.

```cpp
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;
import forge.net.p2p.pubsub;

auto pubsub = context.apis().get<forge::plugins::net::p2p::node::pubsub_api>(
   {.id = {"forge.plugins.net.p2p.node.pubsub"}, .major = 1});

auto subscription = co_await pubsub->subscribe(
   forge::net::p2p::pubsub::topic{.value = "catalog.updates"},
   [](forge::plugins::net::p2p::node::message value)
      -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
      consume(value.data);
      co_return forge::net::p2p::pubsub::validation_result::accept;
   });
co_await pubsub->publish({.value = "catalog.updates"}, std::vector<std::uint8_t>{1, 2, 3});
co_await pubsub->unsubscribe(std::move(subscription));
```

### Read Diagnostics And Host State

```cpp
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.events_api;

auto diagnostics = context.apis().get<forge::plugins::net::p2p::node::diagnostics_api>(
   {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1});
auto network = diagnostics->network();
auto resources = diagnostics->resources();
auto peers = diagnostics->peers({.only_connected = true, .limit = 100});
auto routing = diagnostics->routing_status({.value = "/ipfs/kad/1.0.0"});
auto scores = diagnostics->pubsub_scores();

auto events = context.apis().get<forge::plugins::net::p2p::node::events_api>(
   {.id = {"forge.plugins.net.p2p.node.events"}, .major = 1});
auto subscription = events->host_events();
while (auto state = co_await subscription.async_read()) {
   // Complete native snapshots may coalesce; this is not a delta event log.
   consume_host_state(*state);
}
```

## Security And Boundaries

- The plugin owns network lifecycle and contribution mounting, not product
  authorization decisions.
- Peer identity, protocol negotiation and session mechanics stay in
  `forge_net_p2p`; application plugins use typed contribution APIs.
- Insecure test mode and generated identity material are local-test-only.
- Durable peer history supplies startup candidates, not trusted live routes;
  Kademlia server admission still requires verified Identify or a successful
  DHT exchange.

## Common Mistakes

- Creating a second node in a product plugin instead of requesting the shared
  node API.
- Treating P2P reachability as authorization.
- Publishing unbounded handlers without deadline/frame limits.
- Treating PubSub delivery as durable or encoding product authority in topic
  names alone.
- Treating diagnostics snapshots or source-only/component tests as production
  readiness or live donor interoperability acceptance.

## Tests

- `test_forge_quic_p2p`
- `test_forge_yamux`
- `test_forge_stcp`
- `test_forge_plugins`
- `test_forge_package_plugins_net_p2p_node`
