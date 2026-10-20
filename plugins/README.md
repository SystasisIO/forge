# FORGE Plugins

Official FORGE plugins are reusable infrastructure blocks for `forge_app`
applications. A plugin owns lifecycle, configuration and shared resources, then
publishes narrow typed APIs that application plugins can use without reaching
into low-level transports directly.

Plugins are useful when an application needs one shared service, such as an
HTTP server, a P2P node, a Chain signer or a crypto secrets service, and many
application plugins need to contribute behavior to it.

## When To Use

- A resource has application lifecycle, config and startup/shutdown ordering.
- Multiple application plugins should contribute to one shared service.
- The shared service should expose a narrow local `forge_api_core` contract instead
  of leaking low-level implementation types.
- Operators should configure the service through `plugins.<family>.<name>`.

## When Not To Use

- Do not create a plugin for a pure value type, serialization helper or
  stateless algorithm. Use a library.
- Do not put product business logic, billing, authorization or storage policy
  into official Forge plugins.
- Do not create hidden duplicate runtimes in product plugins when an official
  infrastructure plugin already owns the service.

## How Applications Use Plugins

1. Register official plugin descriptors in the application plugin registry.
2. Let `forge::app::application_shell` collect schema-driven config for the app
   and registered plugins.
3. During `on_initialize()`, application plugins acquire local plugin APIs from
   `plugin_context::apis()`.
4. During startup, infrastructure plugins close contribution windows, start
   their owned resources and expose the composed runtime behavior.

```cpp
registry.register_plugin(forge::plugins::net::http::server::plugin::descriptor());
registry.register_plugin(forge::plugins::net::p2p::node::plugin::descriptor());
registry.register_plugin(forge::plugins::chain::signer::plugin::descriptor());
registry.register_plugin(forge::plugins::crypto::secrets::plugin::descriptor());
registry.register_plugin(forge::plugins::log::otlp::plugin::descriptor());
registry.register_plugin(forge::plugins::db::store::plugin::descriptor());
```

```cpp
auto http = context.apis().get<forge::plugins::net::http::server::api>(
   {.id = {"forge.plugins.net.http.server"}, .major = 2});

co_await http->publish<catalog_api>();
```

Use focused plugin components in small consumers:

```cmake
find_package(Forge REQUIRED COMPONENTS plugins_net_http_server plugins_chain_signer)

target_link_libraries(app PRIVATE
   Forge::forge_plugins_net_http_server
   Forge::forge_plugins_chain_signer)
```

## Available Plugins

| Plugin | Target | Config section | Purpose |
| --- | --- | --- | --- |
| [`forge::plugins::net::http::server`](net/http/server/README.md) | `forge_plugins_net_http_server` | `plugins.net.http.server` | Runs one HTTP server, composes TLS from scoped Secrets material and accepts typed HTTP API and middleware contributions. |
| [`forge::plugins::net::p2p::node`](net/p2p/node/README.md) | `forge_plugins_net_p2p_node` | `plugins.net.p2p.node` | Owns one P2P node with focused connectivity, DHT, PubSub, diagnostics and host-event APIs. |
| [`forge::plugins::net::p2p::resolver`](net/p2p/resolver/README.md) | `forge_plugins_net_p2p_resolver` | `plugins.net.p2p.resolver` | Publishes and resolves peer API metadata over the P2P node. |
| [`forge::plugins::swarm::node`](swarm/node/README.md) | `forge_plugins_swarm_node` | `plugins.swarm.node` | Preview thin Swarm runtime adapter: authenticated P2P discovery and API calls, product-owned file stores/admission, optional named ObjectDB catalog. |
| [`forge::plugins::chain::signer`](chain/signer/README.md) | `forge_plugins_chain_signer` | `plugins.chain.signer` | Composes named transaction and finality providers behind typed Chain signer APIs and exact caller/action policy. |
| [`forge::plugins::crypto::wallet`](crypto/wallet/README.md) | `forge_plugins_crypto_wallet` | `plugins.crypto.wallet` | Owns bounded encrypted wallet providers and authorized local management. |
| [`forge::plugins::crypto::secrets`](crypto/secrets/README.md) | `forge_plugins_crypto_secrets` | `plugins.crypto.secrets` | Provides bounded secret retrieval, derivation and symmetric encryption operations. |
| [`forge::plugins::log::otlp`](log/otlp/README.md) | `forge_plugins_log_otlp` | `plugins.log.otlp` | Exports configured FORGE logger routes to OTLP/HTTP JSON. |
| [`forge::plugins::db::store`](db/store/README.md) | `forge_plugins_db_store` | `plugins.db.store` | Owns configured named DB stores and exposes optional Object, Blob and Revision layers over one Core driver. |

The aggregate target `forge_plugins` and package component `plugins` are
interface convenience dependencies. They own no compiled code and only
propagate the leaf plugin usage requirements. Prefer focused targets/components
in small consumers:

```cmake
find_package(Forge REQUIRED COMPONENTS plugins_net_http_server)
target_link_libraries(app PRIVATE Forge::forge_plugins_net_http_server)
```

## Lifecycle Migration

The approved clean source break makes public `configure/provide/initialize/
after_initialize/startup/request_stop/shutdown` final framework wrappers.
Derived plugins override only protected `on_configure`, `on_provide`,
`on_initialize`, `on_after_initialize`, `on_startup`, `on_request_stop` and
`on_shutdown`, retaining their arguments. Every stop hook is `noexcept`.
Calls through the public wrappers retain their names; never call a hook directly.

Configure, provide and initialize are one-shot input operations. Repeated input
is a typed lifecycle error, not a silent config update. Direct `startup()` first
ensures the same `after_initialize()` preparation barrier used by the application.
Startup and shutdown are joined/idempotent; failed shutdown permits cleanup retry.

The private implementation copies `lifecycle_handle()`; public `lifecycle()`
returns a read-only observer. `snapshot().admission_open` permits resource setup
and is not running readiness. Running-only APIs use the guard overload requiring
`lifecycle_phase::started` while copying their resource. Installation is
synchronous only: no await, lifecycle reentry, virtual calls, teardown or user
callbacks. Lock order is lifecycle then private resource mutex; retired owners
are released after both locks are gone. Backend readiness and cleanup barriers
are not replaced by common lifecycle phases. No extra scheduler is introduced.

## Public Module Shape

Each official plugin follows the same public module layout:

- `forge.plugins.<family>.<name>.plugin` provides `plugin` and its
  `[[nodiscard]] static plugin::descriptor(...)` factory.
  The former `...descriptor` modules, free `descriptor(...)` functions and
  `default_descriptor()` wrappers are removed without aliases. Import
  `...plugin` and call `plugin::descriptor(...)`; configurable factories
  retain their construction parameters and behavior.
- Plugin contracts may be owned by a domain library when they are consumed
  outside the plugin; Chain signer implements `forge.chain.api` contracts.
- `forge.plugins.<family>.<name>.types` provides config and DTO types.
- `forge.plugins.<family>.<name>.exceptions` provides typed exceptions.

Plugins that need an extra public slice, such as HTTP middleware, keep that
slice under the same leaf namespace. Grouping namespaces like
`forge::plugins::net::p2p`, `forge::plugins::net::http`, `forge::plugins::crypto` and
`forge::plugins::db` are empty.

## Boundaries

Official plugins are not mini-applications and do not expose raw lifecycle or
transport mutation APIs. Their public APIs are typed contribution surfaces:

- HTTP APIs are published through `publish<Interface>()`, not raw route verbs.
- P2P APIs are published through typed `forge::api::core::binding_plan` values.
- Config is decoded through `BOOST_DESCRIBE_STRUCT`, `forge_schema` rules and
  `forge_config_core`.
- Product policy, auth, billing, storage semantics and downstream vocabulary do
  not belong in FORGE plugins.

## Security And Redaction

- Plugins that receive provider references must keep private-key material out of
  config and diagnostics.
- Local plugin APIs are not product authorization boundaries by themselves.
  Consumers decide which product code may call them.
- Network-facing plugins must keep limits, deadlines and contribution windows
  explicit.
- Plugin README examples should use neutral catalog/service vocabulary and
  avoid downstream product terms.

## Common Mistakes

- Registering two plugins that own the same runtime resource, such as two shared
  P2P nodes.
- Calling raw transport or crypto internals from product plugins instead of the
  owning plugin API.
- Treating generated config examples as secure secret storage.
- Adding an aggregate-only public module for convenience. Import the concrete
  plugin slices.

## Tests

- `test_forge_plugins`
- `test_forge_swarm_plugin` and its focused MDBX/RocksDB catalog gates
- Focused package tests such as `test_forge_package_plugins_net_http_server`,
  `test_forge_package_plugins_chain_signer`,
  `test_forge_package_plugins_crypto_signer_removed`,
  `test_forge_package_plugins_crypto_secrets`,
  `test_forge_package_plugins_log_otlp`,
  `test_forge_package_plugins_net_p2p_node` and
  `test_forge_package_plugins_db_store` and
  `test_forge_package_plugins_swarm_node_component`.
