# DB Store Plugin

## Ordered opening prerequisite

The canonical plugin constructor and `plugin::descriptor(options)` can receive
owned `open_prerequisite{store, before, check}` records. Empty options preserve
normal/default/custom composition. The complete names/Object-layer/duplicate/
self/cycle graph is validated before any configured native driver opens.
The existing pending batch then opens each prerequisite before its protected
targets and awaits its one callback on the SAME actual ObjectDB owner.
There is no new worker, runtime queue, generally-ready handle or raw-driver API.

The trusted application callback may register its own schema and perform
bounded checks, but the store reference is borrowed only until completion:
it must not retain store copies, transactions, snapshots or tasks. It is called
outside the plugin mutex. Normal success transfers the SAME owners to managed
records without closing/reopening; handles stay unavailable until all checks
and store opens have completed.

On an opt-in failed attempt, acquired owners move without allocation into the
same existing managed records, not into a second registry. The attempt is
terminal and cannot call a check again. Existing uncancelled App rollback owns
shutdown; configured drivers close, caller-owned custom drivers never close.
Unknown cleanup errors retain existing ownership and propagate; the caller
must retain its App/runtime through an explicit cleanup attempt. Ordinary
shutdown's session-busy behavior and strict activation semantics are unchanged.

The exact callback exception is preserved at the Store initialization boundary;
unrelated driver/layer errors retain normal `initialize_failed` wrapping.
The callback is an application-composition admission step, not namespace
freshness, chain-origin or snapshot publication authority. Product policies
and model registration remain outside Forge.

## Проверка пустого ObjectDB без нового владельца транзакции

`objects().require_pristine(active)` и `revisions().require_pristine(active)`
принимают только `transaction&`, полученную через `store.begin_transaction()`
с уже сохранённой ObjectDB-транзакцией. Они удерживают фактический открытый
владелец и переиспользуют тот же ObjectDB participant и single-writer lease
(право единственного писателя), не создавая вторую транзакцию или Revision scope.
Второй вариант проверяет начальное состояние ObjectDB вместе с Revision;
первый — только ObjectDB header. Сканирование и формат принадлежат DB-библиотекам.

```cpp
auto active = co_await store.begin_transaction();
co_await store.objects().require_pristine(active); // Object-only store.
// For an Object+Revision store use revisions().require_pristine(active),
// BEFORE revisions().join(active). Keep SAME active through initialization.
co_await active.rollback(); // Or explicitly commit the caller's later writes.
```

Непустая, закрытая или чужая транзакция, Core-only wrapper без ObjectDB cache
и уже присоединённый Revision scope отклоняются без восстановления или reset.
Имя store не заменяет проверку фактического ObjectDB owner; временная alias
не освобождает исходный lease. Проверка не коммитит и не откатывает транзакцию;
вызывающий обязан завершить её и сохранить App/runtime живыми до окончания.
Ошибки owning DB codec/scanner сохраняются, не превращаются в `false`.

Нейтральный контракт требует actual single-writer, но не transactional ID policy.
Provisioning-продукт отдельно проверяет `objects().runtime_options()` до begin:
`writes == single_writer` и, если это его требование, `id_allocation == transactional`.
Эта проверка не доказывает пустоту Blob/Auth, path reservation, происхождение
цепи, отсутствие прежней истории ключа или готовность к активации каталога.

## Восстановление точных allocator counters

`objects().restore_allocations(active, values)` — тонкая передача owning ObjectDB
`store::restore_allocations` с owned `vector<store::allocation>`. Она удерживает
фактический owner и использует тот же cached ObjectTX, проверку имени/identity и
live Core; Core-only, foreign и attached Revision scope отклоняются. Новый writer,
raw key codec, scanner или отдельный allocator не создаются.

Фактические `single_writer + transactional`, registered-type batch, header-only
namespace, sticky failure/trace и rollback semantics проверяет ObjectDB owner.
Вызов не завершает транзакцию; потребитель сохраняет App/runtime и SAME transaction
до явного commit/rollback. Для нового import stage сначала отключают Revision;
после проверенного Object/Blob/Auth commit и actual close его можно открыть обычным
Store с Revision и проверить local metadata. Bridge не доказывает корректность
snapshot, полноту product manifest, Blob/Auth или готовность directory activation.

## Lifecycle Migration

Public lifecycle entry points are final framework wrappers; implementations use
protected `on_*` hooks with unchanged arguments. See the
[shared migration contract](../../README.md#lifecycle-migration). No compatibility aliases
or plugin-local schedulers are provided.

Common lifecycle is not DB readiness. Private composition states
`collecting/opening/ready/closed` describe physical layers; per-store
`opened/started`, driver ownership and cleanup completion remain backend state.
Shared stop rejects new store handles and registration. Retained handles and
existing flush paths remain available for dependent persistence cleanup until
physical close. This preserved drain policy is not general post-stop admission.

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

## Explicit closed-directory activation

Normal shutdown deliberately permits retained sessions to defer backend close;
it is not a publication/durability fence. Offline provisioning can instead call
`store_handle::close_for_activation()` on a configured plugin-owned MDBX
`durable-sync` store. The App must have its normal bounded compute executor.
Externally supplied drivers, other backends, relative paths and weaker durability
are unsupported, without changing their ordinary Store behavior.

The move-only, non-default-constructible `closed_store` is issued only after
actual backend/lane close. Core `driver_busy` leaves this SAME strict owner
retained and new admissions closed; release old sessions and explicitly retry
close on the SAME handle. Do not flush first: Core close admission is already
closed. Other close/preflight errors remain sticky with the exact owner/error;
they never issue a token or silently retry. An outstanding strict claim prevents
ordinary shutdown from taking its legacy busy-release path.

`closed.activate(absolute_destination)` uses the existing App compute executor,
a private parent ownership lock and descriptor-relative native no-replace rename
(Darwin `renameatx_np(RENAME_EXCL)`, Linux `renameat2(RENAME_NOREPLACE)`).
Stage/target must be siblings on the same filesystem under an owner-only parent.
There is no overwrite, copy, deletion or cross-filesystem fallback. Unsupported
native/filesystem behavior is a typed failure.

After rename but before a successful parent-directory fence, state remains
`published_unconfirmed`; retain the SAME capability and explicitly call
`confirm_durability()`. This does not run the rename again. Keep the App/runtime
alive through the awaited operation; dropping an in-flight capability is a
fail-fast contract violation, not cancellation or implicit cleanup.

`state()` reports exact root name/device/inode, phase and a distinct
`published` flag. Confirming an initially closed stage may make it durable with
`published=false`: durability alone is NOT import/readiness authority.
Consumers must independently match their actual destination identity and their
own verified local intent/baseline before readiness. Provider metadata is never
a capability. Same-UID/root interference, external noncooperating writers,
forked use and cross-root transactions are not supported guarantees.

Focused owning tests are in `tests/plugins_db_store/activation_tests.cpp`;
the installed `plugins_db_store` consumer compiles the same public API. Tests
cover busy/retry/reopen, distinct stage/publication state, no-replace/symlink and
identity guards, postrename fence failure/confirmation and unsupported modes.

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

For configured stores, `create-if-missing: false` also selects noninitializing
Object/Revision layer opens. An existing native database that is missing its
Object header or Revision state is rejected, not repaired. Existing valid layers
remain writable after opening. The effective choice is retained in the SAME
managed record and copied into every pending-open snapshot; it is not inferred
from an optional driver-creation config during replay. Default/true configuration
and programmatic `add_store` keep their existing creating behavior.

This promises no logical Object/Revision initialization/upgrade writes, not zero
native open/lock/WAL/DBI activity. `create-missing-column-families` is a separate
native policy: applications requiring safe existing-root access must explicitly
set BOTH creation flags false. Store does not silently override the family flag.
Noninitializing opens do not prove trusted application state, pristine contents or
custody. Ordinary shutdown/strict activation ownership is unchanged.

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

`authenticated.export_checkpoint(read, root, sink, limits)` accepts a Store
snapshot by value and exports the exact retained root from that SAME Core
snapshot, even after later commits. The ordinary entry point captures base Auth
ownership immediately; its private continuation keeps managed Store state, all
snapshot views and the export task alive through lazy start, sink suspension and
terminal completion. No fresh read or raw Core getter is introduced. The caller
keeps the App/runtime alive through completion and supplies bounded sink policy.

`authenticated.join_restore(tx, root, limits)` is synchronous and delegates
immediately to the base Auth preadmission guard. It accepts a same-origin
Core-only Store transaction as well as the cached ObjectDB transaction; it does
not attach an Object layer, open another writer, await or commit. Named-store
routing is checked first, while base Auth checks actual Core origin, active
writable state, family/domain policy and Revision capture. Identical names do
not grant authority over another driver.

```cpp
auto read = co_await source.begin_read();
auto summary = co_await source_auth.export_checkpoint(read, retained_root, sink, limits);
auto tx = co_await target.begin_transaction();
auto restore = target_auth.join_restore(tx, summary.anchor, limits);
co_await restore.restore_checkpoint(source_frames, verify_all_product_sections);
co_await tx.commit();
```

Restore requires a pristine dedicated authenticated family and no Revision
capture. Its base participant denies commit from admission until a successful
restore plus verifier completion; abandoned work, caught failures and late
Revision capture remain uncommittable. The verifier still owns product-level
validation and outer-stream completion. This bridge proves neither product
snapshot completeness nor directory activation readiness.

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
