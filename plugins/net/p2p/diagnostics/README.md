# P2P Diagnostics Plugin

`forge::plugins::net::p2p::diagnostics` exposes read-only diagnostics for the shared
P2P node. It is intended for operators, tests and application plugins that need
bounded snapshots of network state without depending on private node internals.

## When To Use

- Operators or tests need bounded P2P snapshots through a typed local API.
- A product plugin needs read-only peer/session/resource/pubsub visibility.
- Diagnostics should compose over the shared node without importing private
  `forge_net_p2p` internals.

## When Not To Use

- Do not use diagnostics as a health policy engine or alert router.
- Do not mutate P2P state through this plugin.
- Do not expose unbounded peer/session lists to remote callers.

## Identity

- Target: `forge_plugins_net_p2p_diagnostics`
- Package component: `plugins_net_p2p_diagnostics`
- Plugin id: `forge.plugins.net.p2p.diagnostics`
- Main API id: `forge.plugins.net.p2p.diagnostics`
- Plugin version: `2.0.0`
- Main API contract: `2.0`
- Events API: `forge.plugins.net.p2p.diagnostics.events` (contract `1.0`, local only)
- Node diagnostics source dependency: `forge.plugins.net.p2p.node.diagnostics_source` (contract `2.0`)
- Node events source dependency: `forge.plugins.net.p2p.node.host_event_source` (contract `1.0`)
- Config section: `plugins.net.p2p.diagnostics`
- Depends on plugin id: `forge.plugins.net.p2p.node`
- Public modules:
  - `forge.plugins.net.p2p.diagnostics.plugin`
  - `forge.plugins.net.p2p.diagnostics.api`
  - `forge.plugins.net.p2p.diagnostics.events_api`
  - `forge.plugins.net.p2p.diagnostics.types`
  - `forge.plugins.net.p2p.diagnostics.exceptions`

## What It Provides

- Network snapshot and network-state reads.
- Resource-manager snapshot reads.
- Pubsub snapshot reads when pubsub is enabled.
- Peer listing and single-peer lookup with bounded limits.
- Native host-state reads and subscriptions, including reachability and confirmed addresses.

It is read-only. It does not add HTTP endpoints, logging sinks or product health
semantics by itself.

## Config

```yaml
plugins:
   net:
     p2p:
        diagnostics:
           max-peers: 1024
           max-sessions: 1024
           max-endpoints-per-peer: 64
           max-protocols-per-peer: 128
           max-relay-reservations-per-peer: 64
```

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_plugins_net_p2p_node`
- `forge_config_core`
- `forge_schema`

## Examples

### Read Local Diagnostics

```cpp
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.plugin;

auto diagnostics = context.apis().get<forge::plugins::net::p2p::diagnostics::api>(
   {.id = {"forge.plugins.net.p2p.diagnostics"}, .major = 2});

auto network = diagnostics->network();
auto resources = diagnostics->resources();
auto peers = diagnostics->peers({.only_connected = true, .limit = 100});
```

```cpp
registry.register_plugin(forge::plugins::net::p2p::node::descriptor());
registry.register_plugin(forge::plugins::net::p2p::diagnostics::descriptor());
```

### Observe Host State

```cpp
import forge.plugins.net.p2p.diagnostics.events_api;

auto events = context.apis().get<forge::plugins::net::p2p::diagnostics::events_api>(
   {.id = {"forge.plugins.net.p2p.diagnostics.events"}, .major = 1});
auto subscription = events->host_events();
while (auto state = co_await subscription.async_read()) {
   // Each value is a complete native snapshot, not a delta.
   consume_host_state(*state);
}
```

The first value is the native initial snapshot. Later state changes may coalesce;
preserve `generation` and `resync_required` rather than interpreting this as an
unbounded event log. Each subscription permits one pending reader. Canceling a
read does not close the subscription; `close()` closes that subscription only.

New event operations are rejected after diagnostics `request_stop()`. Issued
subscriptions retain their native lifetime independently of the API handle or
diagnostics plugin, and end when the shared node stops. Diagnostics does not add
a queue or promise to drain a previously admitted source callback at shutdown.
Existing snapshot API 2.0 remains readable until diagnostics shutdown releases
its source. All source calls execute outside the diagnostics mutex.

## Migration From 1.x

Diagnostics peer endpoint records now expose raw `multiaddr` through `.address`
instead of a concrete transport endpoint. Request diagnostics API major `2` and
provide node diagnostics source major `2`; there is no compatibility alias for
the v1 record shape.

## Security And Boundaries

- Diagnostics are local-only API reads. Publishing them remotely is a product
  decision and should apply authorization.
- Config limits bound peer, session, endpoint, protocol and relay snapshots.
- Snapshot output should not include private key material or raw secrets.

## Common Mistakes

- Treating diagnostics as authoritative product readiness.
- Returning full network state without limits.
- Adding mutation helpers here instead of extending the owning P2P node API.

## Tests

- `test_forge_quic_p2p`
- `test_forge_plugins`
