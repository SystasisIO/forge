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
- Plugin version and local API: `2.0.0` / contract `2.0`
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
- Own one native Partial registration per topic, with a required full-message fallback.

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
           partial-messages: false
```

`partial-messages: true` selects native preferred GossipSub v1.3 and enables
the global Partial extension. False preserves the prior v1.1/disabled defaults.
Native version negotiation, bounds and validation are unchanged; wider version,
fallback and scoring configuration is not part of this option.

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
   {.id = {"forge.plugins.net.p2p.pubsub"}, .major = 2});

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

### Partial Registrations

`enable_partial(topic, full_fallback, partial_options, subscribe_options)`
requires receive and gossip callbacks and returns the native `partial_topic`.
The fallback participates in the ordinary validation aggregation and reserves a
handler slot even while enable is pending. It is not returned by `subscriptions()`
and cannot be removed with an ordinary subscription token.

`advertise_partial`, `forget_partial`, `partial_peers` and `send_partial` retain
native registration identity and bounds. Peer snapshots are not lasting send
permission; native send rechecks capability. Applications own group encoding,
reconstruction and revision rules, not this facade.
Admitted sends do not hold the topic transition gate during native I/O. Caller,
registration and plugin stop are composed before native send; downgrade can
cancel an in-flight send without waiting behind it. Shutdown still joins it.

`disable_partial(token)` preserves ordinary handlers. Unsubscribing the last
ordinary handler preserves a current Partial owner. A failed native enable may
already have mutated: compensation restores the existing full-handler mux when
ordinary owners remain, rather than removing their topic. Errors after a
potentially committed downgrade never restore callback admission. The same token
can retry retained cleanup; an actual completed disable makes it stale.

Callbacks receive a scoped stop token composed from native, registration and
plugin stop. Plugin stop reaches active retired callbacks before waiting for
them. Disable requests stop but does not wait for its own callback; shutdown
joins admitted callbacks and capture destruction. Keep stop callbacks nonblocking.
Native callback wrappers own only weak references; no queue or background worker
is introduced.

### Contract Migration

Consumers must request PubSub API major 2 and node `pubsub_source` major 2.
Custom source implementations must implement the six Partial operations; no
default successful stub or compatibility alias is supplied. Existing async source
methods now retain their implementation at call time. The official source uses
running admission for new I/O; full-topic leave still supports stopped local cleanup.

Receive-side donor traceability: pinned Go pubsub `0ed6f6fd`,
`pubsub.go:1390-1430`, replaces per-topic state on each subscribed update and
decodes absent flags as false. Rust `22fb4c78`, `behaviour.rs:2134` and
`extensions/partial_messages.rs:158`, updates the partial subscription before
duplicate subscription filtering. A full-only resubscription therefore needs no
PRUNE. This is source evidence, not a live plugin/donor interoperability claim.

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

## Lifecycle And Concurrency

Async facade calls retain their implementation at call time. Typed publish
serializes its argument immediately; the caller's value need not survive until
the returned awaitable is started. Source calls use an owned snapshot outside
the plugin mutex.

Each topic serializes its native join/leave transitions independently. Pending
subscriptions reserve both topic and handler capacity before waiting. A failed
last leave retains its subscription token and cleanup state: retry unsubscribe
with that token before joining the topic again.

`request_stop()` closes admission, including queued transitions. Shutdown joins
admitted operations and handlers, compensates potentially mutating joins, and
leaves native topics before releasing the source. Concurrent shutdown calls
observe one completion/error. Cleanup failures are retained and rethrown, not
reported as successful disposal. A handler may unsubscribe itself; unsubscribe
does not wait for that handler to return.

Handler deadlines are cooperative: cancellation does not detach a handler that
ignores it. Do not wait for plugin shutdown from inside one of its own handlers.
There is no hard shutdown deadline or background cleanup worker.

## Common Mistakes

- Treating pubsub delivery as durable or exactly-once.
- Running expensive validation handlers without deadlines or active handler
  limits.
- Encoding product authority in topic names alone.

## Tests

- `test_forge_quic_p2p`
- `test_forge_plugins`
- `test_forge_package_plugins_net_p2p_pubsub` (installed consumer)
