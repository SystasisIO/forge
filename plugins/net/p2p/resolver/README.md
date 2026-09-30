# P2P Resolver Plugin

`forge::plugins::net::p2p::resolver` publishes and resolves typed API metadata over
the shared P2P node. It lets consumers ask a peer which API versions and method
contracts it exposes before opening a typed API connection.

## When To Use

- Peers need to discover which typed Forge APIs another peer exposes.
- A product plugin wants `remote<T>()` handles without hardcoding peer-side API
  descriptors.
- API compatibility should be checked before opening a remote typed connection.

## When Not To Use

- Do not use the resolver for peer discovery, routing, relay selection or
  durable service registry policy.
- Do not use it as an authorization check. It reports capabilities, not trust.
- Do not bypass `forge.plugins.net.p2p.node`; this plugin depends on the shared
  node.

## Identity

- Target: `forge_plugins_net_p2p_resolver`
- Package component: `plugins_net_p2p_resolver`
- Plugin id: `forge.plugins.net.p2p.resolver`
- Main API id: `forge.plugins.net.p2p.resolver`
- Main API contract: `2.0`
- Extra protocol API id: `forge.plugins.net.p2p.resolver.protocol`
- Resolver protocol route: `/forge/api/resolver/2`
- Config section: `plugins.net.p2p.resolver`
- Depends on plugin id: `forge.plugins.net.p2p.node`
- Public modules:
  - `forge.plugins.net.p2p.resolver.plugin`
  - `forge.plugins.net.p2p.resolver.descriptor`
  - `forge.plugins.net.p2p.resolver.api`
  - `forge.plugins.net.p2p.resolver.managed_api`
  - `forge.plugins.net.p2p.resolver.types`
  - `forge.plugins.net.p2p.resolver.exceptions`

## What It Provides

- Publishes local API descriptors under a P2P protocol id.
- Queries peer API descriptors with bounded response validation.
- Resolves a requested `forge::api::core::api_ref` to a concrete peer descriptor.
- Opens typed remote API handles with compatibility projection.
- Opens stable managed handles over an ordered peer set with sticky selection,
  single-flight reconnect and bounded backoff.

## Publication Ownership

`api::publish_api(...)` returns a move-only `forge::api::p2p::publication`
lease. Keep this lease alive while the route should remain advertised. `close()`
stops admission synchronously; `async_close()` additionally waits for admitted
node sessions to drain.

Publishing the same protocol replaces its current generation atomically. A
lease for the old generation cannot close or remove the replacement. API ids
remain unique across different protocols.

`max-apis-per-peer` also bounds retained closing local generations.
Replacements fail closed with a typed resolver error when that backlog reaches
the bound; completed retirement or shutdown fallback drains it. This prevents
unbounded retention if the background retirement queue cannot admit work.

The resolver API contract is now `2.0` and requires
`forge.plugins.net.p2p.node` major `2`. Consumers upgrading from resolver `1.x`
must request resolver major `2`, retain the returned publication lease, and
explicitly close or await it during their own shutdown.

It does not own the P2P node lifecycle; it composes through
`forge.plugins.net.p2p.node`.

## Config

```yaml
plugins:
   net:
     p2p:
        resolver:
           protocol-id: /forge/api/resolver/2
           cache-ttl-ms: 60000
           query-deadline-ms: 5000
           open-deadline-ms: 10000
           max-cached-peers: 4096
           max-apis-per-peer: 1024
           max-methods-per-api: 256
           max-errors-per-method: 64
           managed:
              max-peers: 64
              max-remotes: 1024
              max-waiters: 1024
```

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_net_p2p`
- `forge_plugins_net_p2p_node`
- `forge_config_core`
- `forge_schema`

## Examples

### Publish And Resolve A Typed API

```cpp
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.plugin;
import forge.api.p2p.publication;

class catalog_resolver_plugin final : public forge::app::plugin {
 public:
   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::net::p2p::resolver::api>(
         {.id = {"forge.plugins.net.p2p.resolver"}, .major = 2});

      auto plan = forge::api::core::binding()
         .serve(context.apis())
         .export_api<catalog_api>()
         .build();

      publication_ = resolver->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/catalog/api/1"});
      co_return;
   }

 private:
   forge::api::p2p::publication publication_;
};
```

```cpp
auto catalog = co_await resolver->remote<catalog_api>(peer);
auto value = co_await catalog->read(request);
```

### Publish A Managed Process Service

Applications obtain `managed_api` during `builder.connect(...)` and publish the
resulting exact-type service through Forge App:

```cpp
auto resolver = context.api<forge::plugins::net::p2p::resolver::managed_api>();
auto catalog = co_await resolver->remote<catalog_api>(ordered_authoritative_peers);
context.publish(std::make_shared<catalog_service>(std::move(catalog)));
```

The initial connect tries peers in declared order. A live peer remains sticky.
If a call fails at the transport/session boundary, that call is returned to the
caller unchanged and is never replayed; only the next call reconnects, starting
with the next peer. Products must recover an uncertain transaction submission by
transaction ID and verified finality rather than by resubmitting it.

## Security And Boundaries

- Descriptor data is bounded by config limits before it is accepted.
- The resolver projects API compatibility. Caller trust, admission and
  authorization remain product policy.
- Remote errors should be surfaced as typed API errors, not as raw transport
  messages.
- Managed reconnect never changes authorization or trust policy; the caller
  supplies the already approved ordered peer set.

## Common Mistakes

- Treating a successful descriptor query as proof that the peer is trusted.
- Publishing every local API by default. Export only the intended remote
  contract.
- Making cache TTLs unbounded and hiding stale descriptors.

## Tests

- `test_forge_quic_p2p`
- `test_forge_plugins`
- `test_forge_p2p_managed_remote`
