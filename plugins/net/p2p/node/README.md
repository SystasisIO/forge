# P2P Node Plugin

`forge::plugins::net::p2p::node` owns one shared `forge_net_p2p` node and exposes typed
contribution APIs for protocol handlers and API-over-P2P publication.

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

Insecure memory mode remains an explicit local-test path only. Current support
classifications live
in
[`p2p_feature_inventory.json`](../../../../tests/libp2p_interop/p2p_feature_inventory.json);
isolated codec and interop fixtures do not promote this plugin to production.

## When To Use

- A Forge application needs one shared P2P node managed by `forge_app`.
- Product plugins need to publish protocol handlers or typed APIs over the same
  peer/session substrate.
- Diagnostics, resolver and pubsub plugins should compose over one node instead
  of creating parallel network stacks.

## When Not To Use

- Do not create product routing, durable queue or authorization policy in this
  plugin.
- Do not instantiate raw `forge::net::p2p::node` separately inside application
  plugins.
- Do not enable insecure test mode outside local tests.

## Identity

- Target: `forge_plugins_net_p2p_node`
- Package component: `plugins_net_p2p_node`
- Plugin id: `forge.plugins.net.p2p.node`
- Main API id: `forge.plugins.net.p2p.node`
- Plugin version: `6.0.0`
- Main API contract: `2.0`
- Extra API ids:
  - `forge.plugins.net.p2p.node.dht` (contract `1.0`, local only)
  - `forge.plugins.net.p2p.node.diagnostics_source` (contract `2.0`)
  - `forge.plugins.net.p2p.node.pubsub_source`
- Config section: `plugins.net.p2p.node`
- Public modules:
  - `forge.plugins.net.p2p.node.plugin`
  - `forge.plugins.net.p2p.node.descriptor`
  - `forge.plugins.net.p2p.node.api`
  - `forge.plugins.net.p2p.node.dht_api`
  - `forge.plugins.net.p2p.node.types`
  - `forge.plugins.net.p2p.node.exceptions`

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
- Delegates profile-aware peer/provider discovery, owned provider publication,
  validated value operations and typed IPNS creation through `dht_api`.
- Provides internal source APIs used by focused diagnostics and pubsub plugins.

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
retained so a subsequent `shutdown()` can retry deterministic close; the plugin
never drops a persistence backend that still reports pending close work.
An empty active session set is not treated as proof that STCP/Yamux cleanup has
completed.

The plugin does not implement product routing policy or durable application
queues. Core libp2p-style mechanics belong to `forge_net_p2p`; this plugin composes
that substrate for applications.

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
the replacement. Raw `publish_protocol(...)` remains a pre-start-only route
contribution and cannot share a protocol id with an API publication.

Plugin 6.0 upgrades `forge.plugins.net.p2p.node.diagnostics_source` to contract
`2.0`: diagnostics endpoint records now carry raw `multiaddr` values in their
`address` field so DNS address carriers survive. Consumers must request major
`2`; no v1 alias is provided. The private ObjectDB P2P cache marker is v3;
v2 caches must use the existing recoverable `schema-policy: reset` path rather
than being hydrated.

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_net_p2p`
- `forge_api_transport`
- `forge_config_core`
- `forge_schema`
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
         {.id = {"forge.plugins.net.p2p.node"}, .major = 2});

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
registry.register_plugin(forge::plugins::db::store::descriptor());
registry.register_plugin(forge::plugins::crypto::secrets::descriptor());
registry.register_plugin(forge::plugins::net::p2p::node::descriptor());
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

## Tests

- `test_forge_quic_p2p`
- `test_forge_yamux`
- `test_forge_stcp`
- `test_forge_plugins`
- `test_forge_package_plugins_net_p2p_node`
