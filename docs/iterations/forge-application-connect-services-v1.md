# Forge Application Connect Services v1

Status: implemented in `forge_app`.

Related boundaries:

- [Application runtime](../../libraries/app/README.md);
- [Runtime and application lifecycle](../runtime/asio-app.md);
- [Connect services donor baseline](../donors/forge-application-connect-services-v1.md);
- [API Core](../../libraries/api/core/README.md).

## Problem

`application_builder::provide` runs before plugins publish their local APIs.
`after_initialize` runs late enough to consume those APIs, but deliberately
receives a read-only application context. Before this iteration, an application
could not perform one asynchronous composition step that:

1. obtains an API exposed by an initialized plugin;
2. connects a remote client;
3. publishes the resulting process-local client for plugin startup.

Creating a product plugin whose only purpose is to register that client adds a
false runtime owner. Re-exporting remote API proxies also exposes a lower-level
unverified surface instead of the client selected by the composition root.

## Decision

Add one application-owned `connect` phase and a bounded exact-type service
registry. The intended product composition remains short:

```cpp
builder.connect(
   [config, finality, projections]
   (forge::app::connect_context& context)
      -> boost::asio::awaitable<void>
   {
      auto resolver =
         context.api<forge::plugins::net::p2p::resolver::api>();

      auto client =
         co_await forge::chain::api::connect_verified(
            *resolver,
            {
               .peer = config.peer,
               .chain = config.chain,
               .state_domain = config.state_domain,
               .finality = finality,
               .projections = projections,
               .proof_limits = config.proof_limits,
               .service_limits = config.service_limits,
            });

      context.publish(std::move(client));
   });
```

A consumer obtains the exact published type:

```cpp
auto chain = context.service<forge::chain::api::verified_client>();
```

The Chain and P2P names above are a consumer example, not dependencies of
`forge_app`. The connection helper belongs to its integration owner. App owns
only lifecycle, exact-type publication and lookup.

## Public Shape

The target application surface is:

```cpp
class connect_context {
 public:
   template <typename Interface>
   forge::api::core::handle<Interface>
   api(forge::api::core::api_ref requested = Interface::ref()) const;

   template <typename Service>
   void publish(std::shared_ptr<Service> service);

   template <typename Service>
   std::shared_ptr<Service> service() const;
};

class service_view {
 public:
   template <typename Service> std::shared_ptr<Service> get() const;
};

template <typename Handler>
application_builder& application_builder::connect(Handler&& handler);
```

The semantic contract is fixed:

- lookup is by exact C++ type, without string keys;
- one non-null service may be published for each exact type;
- duplicate publication is a typed configuration error;
- missing lookup is a typed unavailable error;
- publication is process-local and never creates an API route;
- publication closes when all connect callbacks complete;
- the registry is immutable before plugin startup begins.
- a `service_view` is a non-owning view of the application registry and must not
  outlive its `application_shell`.

This is not constructor injection, auto-wiring or a dependency graph. App does
not construct arbitrary services, infer dependencies, create scopes or select
implementations. The application composition root creates a small number of
long-lived runtime clients explicitly and publishes them once.

## Lifecycle

The accepted lifecycle order is:

```text
collect config
configure application
configure plugins
provide application APIs
provide plugin APIs
initialize plugins
connect application services
run application after_initialize callbacks
mark initialized
startup plugins
run foreground work
shutdown plugins in reverse order
destroy connected services
```

Consequences:

- `connect` may consume APIs published by plugins and resources created during
  plugin initialization;
- plugins must not require connected services during `initialize`;
- plugins may resolve connected services during `startup`;
- no request can observe a partially published service set;
- connect failure keeps the application out of `initialized` and `startup`;
- rollback shuts down initialized plugins before connected services are
  destroyed, so plugin cleanup may still use them.

`after_initialize` remains read-only. It may inspect connected services but
does not publish additional ones.

## Connection Ownership

The published client is a stable runtime object. Transport connection state,
peer reselection, reconnect, cancellation and retry policy stay inside that
client and its transport dependencies. The application must not replace the
published object on every reconnect.

For a verified chain client:

- the P2P node owns physical connections;
- the resolver selects a peer capability and opens typed remote API handles;
- the verified client owns trust, proof and finality checks;
- a submission client reports transport acceptance only;
- verified transaction status determines inclusion or finality;
- application and product plugins do not receive an unverified fallback.

Remote API handles used to construct a verified client remain private to that
client. Installing them into the local API registry is unnecessary and would
make accidental verification bypass easier.

## Boundaries

- `forge_app` must not import Chain, P2P, HTTP or another concrete transport.
- `connect_context::api` reads the existing local Forge API registry; it does
  not resolve a remote API by itself.
- `publish` stores ordinary long-lived runtime objects, not API descriptors or
  transport routes.
- `forge::plugins::net::p2p::resolver::publish_api` remains server-side network
  export and is unrelated to process-local service publication.
- A product plugin must own real behavior. A plugin whose only job is to place
  a client in a registry is forbidden.
- Hot paths perform only a local exact-type lookup, normally once during plugin
  startup. They do not repeat peer resolution or application wiring.

## Validation

Focused `forge_app` tests must prove:

- connect runs after all plugin `initialize` calls and before every plugin
  `startup` call;
- async and synchronous callbacks run in declaration order;
- a connect callback can consume a plugin-provided API;
- a service published by connect is visible during plugin startup;
- null, duplicate and missing services produce typed errors;
- services cannot be published after the connect phase closes;
- connect failure prevents startup and performs deterministic rollback;
- shutdown destroys connected services only after plugin shutdown;
- repeated `initialize` and shutdown preserve existing idempotency guarantees;
- `forge_app` gains no dependency on P2P or Chain libraries.

An integration fixture should construct a reconnecting client through a fake
resolver API, publish it in three builder statements and consume it from a
plugin startup method. Product-specific Chain behavior belongs to the owning
Chain/P2P tests, not `test_forge_app`.
