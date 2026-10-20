# Swarm Node

Preview C++ API, target `forge_plugins_swarm_node`, component
`plugins_swarm_node`, local contract `forge.plugins.swarm.node` v1.
The plugin composes the native Swarm engine with official P2P APIs and an
optional guarded ObjectDB catalog. It adds no transfer protocol, scheduler,
pool, peer cache, picker, payment mechanism or operation registry.

## Composition

Register `plugin::descriptor(plugin_options)` after its official P2P Node and
DB Store dependencies. The descriptor declares both dependencies because App
does not support optional dependency edges. File mode opens no additional
Swarm catalog; the P2P stack still has its own DB/Secrets dependencies.

`plugin_options` supplies native `engine_options`, optional CPU execution and
product `admission`. CPU defaults to App compute and fails if absent. Object
mode requires an explicit caller-owned `database` executor: RocksDB durable
flush can block. No executor or payload store is created or stopped here.

Configuration section `plugins.swarm.node` has `network.profile`,
`network.application-protocol`, `metadata.mode` (`file`/`object`),
`metadata.object-name`, `metadata.max-inflight` (1..32), and `free-admission`.
Programmatic `initial_config` supplies defaults, overridden by explicit config.
Only `/forge/swarm/1.0.0` is currently accepted as the application protocol;
this is distinct from the DHT profile. Free access is false by default.
Missing admission fails startup closed. Free plus an injected policy is rejected.
Realm separates discovery, never permission.

```cpp
auto options = forge::plugins::swarm::node::plugin_options{};
options.initial_config.free_admission = true; // Explicit free service only.
registry.register_plugin(forge::plugins::swarm::node::plugin::descriptor(options));

// In a dependent application plugin, after infrastructure startup:
auto swarm = context.apis().get<forge::plugins::swarm::node::api>(
   {.id = {"forge.plugins.swarm.node"}, .major = 1});
auto sharing = co_await swarm->async_share(descriptor, product_owned_store, "public");
// Retain sharing for the desired serving lifetime, then join withdrawal.
co_await sharing.async_close();
```

Link the focused installed component:
`find_package(Forge REQUIRED COMPONENTS plugins_swarm_node)` and
`Forge::forge_plugins_swarm_node`. Product admission is supplied programmatically
instead of `free_admission`; payment credentials do not belong in this config.

The local API returns native move-only sharing/download handles and delegates
optional catalog CRUD, indexed pages and checkpoint capabilities. Products
provide and own `core::store`. `make_route(peer, addresses)` and discovery
produce the same bounded, passive, repeatable owned route. Its awaitable retains
context even after the route facade is destroyed. It retains every supplied
address (including carrier hints); native P2P alone performs dialing/fallback.
Context bounds use native address-resolution defaults (100 roots, 4096 bytes
per root); lookup also enforces its requested provider count. A route key is
diagnostic, not authenticated identity. All opens require authenticated target
peer evidence before converting to API transport; only after success is the
principal derived as P2P peer / SHA256(canonical Peer-ID bytes), matching the
official binding. A setup deadline is not the exchange stream's total deadline.

Provider keys are standard SHA2-256 multihashes of canonical Forge Raw
`tuple(string("forge.swarm.providers/v1"), string(realm), swarm_id)`.
There is no second hash of the digest. The application wire remains Forge
Swarm API, not IPFS/Bitswap or BitTorrent wire compatibility.

## Ownership

After initialization, object mode registers catalog models through a guarded
named object handle. Startup opens the catalog after the DB dependency has
started, constructs the engine, then publishes generated `api::peer`.
The access adapter retains the named handle and DB API; durable flush calls
`flush(the_same_name, true)`. It never escapes a raw store or driver.

Publication loss/replacement uses `publication::stop_token()` to cancel its
exact engine, including when no inbound call is active. Request-stop closes
publication admission, cancels the engine and signals capability calls. Each
capability uses one joined child on the caller's executor/strand, with native
Asio cancellation and notification-based stop/deadline wakeups. A late
connection/registration is closed/withdrawn before reporting cancellation.
Primary and cleanup failures are both retained in a typed exception.

Shutdown joins publication sessions, then engine workers/providers/calls/store
leases/checkpoint work, then logically closes/drains the catalog. It does not
close shared stores, P2P, DB or App execution. Escaped checkpoint handles reject
new operations after catalog closure. Accepted but unstarted catalog awaitables
must be awaited or destroyed before shutdown can drain them. Explicit async
shutdown is required before plugin destruction; destructors are not a drain.

## Validation And Limits

`tests/swarm_plugin` contains focused config/lifecycle/capability tests and
official P2P tests using real file stores: free/funded transfer, metadata/data
denial, trusted caller/quota facts, route lifetime and publication replacement
with provider withdrawal. An independent third node downloads through DHT
without an explicit route to the provider. Only identity provisioning and the
unused file-mode DB dependency slot are doubles in these network fixtures.
The late-provider test explicitly uses a cancellation-ignoring DHT double to
exercise ownership, not discovery. Separate paused-connection tests prove both
parent Asio cancellation and plugin lifetime stop, joined late close and
preservation of caller cancellation history.

`test_forge_swarm_plugin_metadata_mdbx` and
`test_forge_swarm_plugin_metadata_rocksdb` use the real DB Store, Secrets, P2P
and Swarm plugins with insecure mode disabled, separate named caches, indexed
pages and durable restart/resume. They check that stopping Swarm leaves shared
DB and execution usable and that old checkpoint capabilities cannot revive.
The installed consumer is `test_forge_package_plugins_swarm_node_component`.

Allocator fault injection at child launch and the full PR8 multi-process/
transport matrix are not claimed by this focused slice. Synthetic quota
policies are not a real payment implementation. Native hint/deadline/relay
acceptance has separate P2P fixtures; Swarm does not add or patch a libp2p wire
protocol.
