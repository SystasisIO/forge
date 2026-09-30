# P2P Pubsub Plugin

`forge::plugins::net::p2p::pubsub` exposes a typed application facade for topic
publish/subscribe over the shared P2P node.

## When To Use

- Application plugins need topic publish/subscribe over the shared P2P node.
- Handlers need async validation and bounded active handler limits.
- Pubsub state should be exposed through a local typed API rather than direct
  network internals.

## When Not To Use

- Do not use this plugin as a durable queue, replay log or product event store.
- Do not put business fan-out, settlement or authorization policy here.
- Do not use arbitrary unbounded topic names or message sizes.

## Identity

- Target: `forge_plugins_net_p2p_pubsub`
- Package component: `plugins_net_p2p_pubsub`
- Plugin id: `forge.plugins.net.p2p.pubsub`
- Main API id: `forge.plugins.net.p2p.pubsub`
- Config section: `plugins.net.p2p.pubsub`
- Depends on plugin id: `forge.plugins.net.p2p.node`
- Public modules:
  - `forge.plugins.net.p2p.pubsub.plugin`
  - `forge.plugins.net.p2p.pubsub.descriptor`
  - `forge.plugins.net.p2p.pubsub.api`
  - `forge.plugins.net.p2p.pubsub.types`
  - `forge.plugins.net.p2p.pubsub.exceptions`

## What It Provides

- Publish raw byte messages to a topic.
- Publish described/serializable values through the typed helper overload.
- Subscribe with async validation handlers.
- Track subscriptions and expose bounded pubsub snapshots.

It does not provide durable queues, replay, business-level fan-out or product
delivery guarantees. Those policies belong above this plugin.

## Config

```yaml
plugins:
   net:
     p2p:
        pubsub:
           max-topics: 1024
           max-handlers-per-topic: 64
           max-active-handlers: 4096
           max-message-size: 1048576
           handler-deadline-ms: 5000
           allowed-topics: []
           denied-topics: []
           sign-publishes: true
```

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_net_p2p`
- `forge_plugins_net_p2p_node`
- `forge_config_core`
- `forge_schema`

## Examples

### Subscribe And Publish

```cpp
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.plugin;

auto pubsub = context.apis().get<forge::plugins::net::p2p::pubsub::api>(
   {.id = {"forge.plugins.net.p2p.pubsub"}, .major = 1});

auto subscription = co_await pubsub->subscribe(
   forge::net::p2p::pubsub::topic{.value = "catalog.updates"},
   [](forge::plugins::net::p2p::pubsub::message value)
      -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
      consume(value.data);
      co_return forge::net::p2p::pubsub::validation_result::accept;
   });

co_await pubsub->publish(
   forge::net::p2p::pubsub::topic{.value = "catalog.updates"},
   std::vector<std::uint8_t>{1, 2, 3});
```

```cpp
registry.register_plugin(forge::plugins::net::p2p::node::descriptor());
registry.register_plugin(forge::plugins::net::p2p::pubsub::descriptor());
```

## Security And Boundaries

- Topic allow/deny lists and max message size are config controls.
- Message validation callbacks return terminal `accept`, `reject` or `ignore`,
  or transient `retry`. Handler deadlines, failures and local active-handler
  backpressure map to bounded retries; they are not product authorization by
  themselves.
- `message.source` is the immediate peer for the current gossip hop.
  `message.author` is present only for a successfully verified signed message.
  Use `source` for per-peer quotas and a present `author` for signed-origin
  attribution or signature policy. Unsigned messages never expose their
  unverified wire `from` value as `author`.
- Signing publish messages is transport/pubsub integrity support, not business
  trust policy.

## Common Mistakes

- Treating pubsub delivery as durable or exactly-once.
- Running expensive validation handlers without deadlines or active handler
  limits.
- Encoding product authority in topic names alone.

## Tests

- `test_forge_quic_p2p`
- `test_forge_plugins`
