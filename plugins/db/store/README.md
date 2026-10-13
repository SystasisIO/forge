# DB Store Plugin

## Logging

`forge.db.store` owns plugin composition/ownership events, not native DB
commits. INFO covers plugin start/stop; DEBUG covers configuration, external
registration and composition readiness. `plugin.stopped` is emitted once after
successful shutdown; repeated shutdown is quiet, and a failed cleanup does not
mark shutdown complete or prevent a later cleanup attempt. `composition.failed`
includes the logical store name and backend so one failed configured store is
identifiable. Diagnostic context refers to existing owned records without
allocating copies before the best-effort logging macro.
No physical path, record key, payload or unsafe exception text is logged.

Log delivery and last-owner resource destruction occur outside the plugin
mutex. Programmatic drivers remain caller-owned (`owns_driver=false`) and
are never closed by plugin shutdown. Configured drivers are plugin-owned;
busy close with surviving sessions is not a warning. The plugin has no OTLP
dependency: an application can configure the existing Forge routes/sinks
before DB initialization, including an optional exporter owned by that app.

## Ownership

`forge::plugins::db::store` owns configured named physical DB stores for
applications. Each named store owns one `forge::db::core::driver` and may expose
`forge::db::object::store`, `forge::db::blob::store` and
`forge::db::revision::store` as optional logical layers.

- Target: `forge_plugins_db_store`
- Package component: `plugins_db_store`
- Plugin id: `forge.plugins.db.store`
- Main API id: `forge.plugins.db.store`
- Config section: `plugins.db.store`
- Public modules:
  - `forge.plugins.db.store.plugin` owns `plugin::descriptor()` and the canonical factory.
  - `forge.plugins.db.store.api`
  - `forge.plugins.db.store.types`
  - `forge.plugins.db.store.exceptions`

## Stability

The DB Store plugin C++ API and local API contract are **Preview** in Forge 8.x.
They may receive documented source-incompatible refinements in a MINOR release.
Configured database layouts and persisted records remain separate compatibility
boundaries.

The plugin handles physical store setup, lifecycle, status and flushing. It does
not describe C++ object schemas or blob retention policy in YAML. Domain code
still declares object/index descriptors in C++ and registers them on
`store_handle.objects()`.

## Config

```yaml
plugins:
  db:
    store:
      stores:
        - name: "witness"
          driver: "rocksdb"
          path: "./data/rocksdb/witness"
          families:
            - "authenticated-state"
          object:
            family: "objectdb"
            write-policy: "single-writer"
            id-allocation: "monotonic"
          blob:
            data-family: "blobdb.data"
            refs-family: "blobdb.refs"
            data-blobs:
              enable-blob-files: true
              min-blob-size: 4096
          revision: {}
```

`revision: {}` explicitly enables durable revisions. It requires `object:`,
uses the same Object family for its system rows and does not create another
RocksDB column family. Omitting `revision:` leaves the layer disabled.

`families` declares additional physical DB Core families used by transaction
participants that share this store but are not ObjectDB or BlobDB layers. Names
must be non-empty and distinct from every configured ObjectDB and BlobDB family.

MDBX is configured on the same named-store surface:

```yaml
plugins:
  db:
    store:
      stores:
        - name: "catalog"
          driver: "mdbx"
          path: "./data/mdbx/catalog"
          object:
            family: "objectdb"
          blob:
            data-family: "blobdb.data"
            refs-family: "blobdb.refs"
          revision: {}
          mdbx:
            durability: "durable-sync"
            max-readers: 256
            map:
              upper-size: 68719476736
              growth-step: 268435456
            lane:
              max-pending-operations: 1024
              max-waiting-submissions: 1024
              thread-name: "db-catalog"
```

`durable-sync` is the safe default. `safe-nosync` is an explicit replayable
state policy and may lose the unsteady committed tail after an operating-system
crash. Each configured MDBX store owns one bounded affine lane and one exclusive
MDBX environment. RocksDB Blob-file options are rejected for MDBX; Blob payloads
remain ordinary records in this backend.

`driver: rocksdb` and `driver: mdbx` are available when Forge is built with the
corresponding DB backend. RocksDB remains the default configured driver. Custom
drivers are added programmatically through the local API during plugin
`initialize()`, before the DB Store `after_initialize()` phase opens every
configured layer.

## Usage

```cpp
auto db = context.apis().get<forge::plugins::db::store::api>(
   forge::plugins::db::store::api::ref());

auto witness = co_await db->store("witness");
witness.objects().register_object<witness_object>();

auto tx = co_await witness.begin_transaction();
auto revision = co_await witness.revisions().join(tx);
auto objects = co_await witness.objects().join(tx);
auto blobs = witness.blobs().join(tx);

auto content = co_await blobs.put(bytes);
co_await objects.insert(witness_record{.content = content});
co_await tx.commit();
```

`begin_transaction()` does not create a revision automatically. Callers opt in
with `revisions().join(tx)` before the first mutation or savepoint. The returned
scope exposes its candidate revision ID, while commit ownership remains with the
plugin transaction. `revisions().revert(tx, head)` and
`revisions().prune_through(tx, boundary, limits)` likewise operate only on a
transaction created by the same named store.

When the named store has an Object layer, `begin_transaction()` reserves that
layer's writer lane. `objects().join(tx)` reuses the already attached Object
participant. The first `blobs().join(tx)` attaches Blob state to the same Core
transaction and later joins reuse that participant. A transaction created by
another named store is rejected.

Use `add_store(name, driver, options)` during setup when an application provides
its own `forge::db::core::driver`. Once every plugin has initialized, DB Store
opens its drivers and logical layers and enters the private `ready` phase.
Application `after_initialize` callbacks can then obtain handles and register
object models before startup:

```cpp
builder.after_initialize([](const forge::app::application_context& context)
                            -> boost::asio::awaitable<void> {
   auto db = context.api_view().get<forge::plugins::db::store::api>(
      forge::plugins::db::store::api::ref());
   auto witness = co_await db->store("witness");
   witness.objects().register_object<witness_object>();
});
```

`status().stores[i].started` remains false in `ready` and becomes true only
after plugin startup. A configured MDBX store also reports its exact
`durability` mode; programmatic custom drivers report no durability because
DB Store cannot infer an external driver's acknowledgement contract. In
`ready`, `objects()` permits only object registration,
interceptor registration and observer registration. Transactions, reads,
writes, indexes, Blob access and flushes remain unavailable until startup. New
stores are rejected from `ready` onward, while opened handles remain available
until shutdown closes the physical store. Revision access is also runtime-only:
`revisions()` throws the typed stopped error in `ready` and becomes available in
`started` and `stopping`. A store without the layer reports
`unavailable_layer`.

Revision state, entries and deltas are DB Object system models. They are
readable through the configured Object store, but application Object mutation
APIs cannot modify them. Object, Blob and Revision operations joined to one
plugin transaction commit or roll back through the same Core driver.

## Authenticated State

`store_handle::authenticated(config)` creates a typed
`forge.db.authenticated` view over the same physical named store. The
application owns the authenticated family/domain policy, while DB Store keeps
the physical driver private:

```cpp
auto authenticated = state.authenticated({
   .family = forge::db::core::family{"state.authenticated"},
   .domain = "spine.consensus.state",
});

auto tx = co_await state.begin_transaction();
auto revision = co_await state.revisions().join(tx);
auto tree = co_await authenticated.join(tx, revision.id());
auto objects = co_await state.objects().join(tx);

// Apply ObjectDB mutations, project the final change set, then stage one root.
co_await tree.stage(projected_mutations, expected_root);
co_await tx.commit();
```

The authenticated handle provides versioned reads, point/range proofs and
bounded pruning. `join` and `prune_through` reject a transaction created by a
different named store. Attach Revision and authenticated participants before
the first application mutation; DB Store remains the only owner of commit and
rollback.

`store_handle::create_checkpoint(path)` forwards the neutral Core checkpoint
boundary to the physical driver. It creates one backend-owned durable copy and
does not reconstruct a database through Object or Blob iteration. Drivers that
do not support checkpoints fail with the Core typed error.

## Shared Reads

`store_handle::begin_read()` opens one Core snapshot and eagerly binds every
configured Object and Blob layer to it:

```cpp
auto read = co_await witness.begin_read();
auto objects = read.objects();
auto blobs = read.blobs();

auto metadata = co_await objects.get(file_id);
auto payload = co_await blobs.get(metadata.content);
```

Both views observe the same committed point, including Object indexes and Blob
owner references. `objects()` or `blobs()` reports `unavailable_layer` when the
named store does not configure that layer. The wrapper deliberately does not
expose its raw Core snapshot.

The Object handle forwards ranked index operations without creating plugin
state or a second transaction boundary. The same descriptor can be queried
through a direct handle, a plugin transaction or the Object view of a unified
read snapshot:

```cpp
auto usage = witness.objects().index<upload_object, by_state>();
auto active = co_await usage.equal_range(upload_state::active).count();
auto bytes = co_await usage.equal_range(upload_state::active)
   .sum<by_payload_bytes>();

auto read = co_await witness.begin_read();
auto stable_rank = co_await read.objects()
   .index<upload_object, by_state>()
   .lower_bound_rank(upload_state::active);
```

New reads are accepted only in `started` and `stopping`. A snapshot opened
before shutdown owns its backend session and remains usable until its last copy
is destroyed, even after plugin shutdown. Configured MDBX stores defer physical
environment/lane cleanup when such a session is still alive. Keep snapshots
operation-scoped or bounded: old RocksDB versions and Blob files or old MDBX
MVCC pages remain live while a snapshot is held.

Configured drivers are plugin-owned and are closed during shutdown. A live
session makes close fail fast internally, keeps its backend operational and
finishes physical cleanup when the last session is released. Drivers supplied
through `add_store()` remain caller-owned: the plugin never closes them or
their execution runtime.
