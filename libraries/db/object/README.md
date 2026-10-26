# forge_db_object

`forge_db_object` is the typed object/index engine layer for Forge. It owns
object descriptors, primary and secondary index maintenance, object
serialization, async transactions and stable read snapshots.

## Logging

The private `forge_log` dependency uses `forge.db.object`. DEBUG
`store.initialized` identifies the selected writer/id policy; `changes.committed`
reports the actual mutation count after an owned transaction has completed all
Core hooks and released writer gates. Joined transactions committed through
another owner do not duplicate that commit event. Header rejection emits only
a constant reason after successful rollback and gate release. Keys, objects,
family names and exception text are not logged. Backend physical outcomes
remain backend-owned; no Object log is emitted while native writes are active.

## Stability

The DB Object public C++ API is **Preview** in Forge 8.x. It may receive
documented source-incompatible refinements in a MINOR release. Persisted object
and index layouts are a separate compatibility boundary and are not made
unstable by this source API status.

It is backend-free. Physical persistence is supplied by a shared
`forge::db::core::driver`, so DB Object can run over in-memory test drivers, RocksDB
through `forge_db_rocksdb`, and future backends without importing them.

## Layering

- `forge::db::core`: low-level record driver, transaction and snapshot contract.
- `forge::db::object`: typed objects, indexes, hooks and store/transaction API.
- `forge::db::rocksdb`: RocksDB implementation of the shared
  `forge::db::core::driver` contract.
- `plugins.db.store`: application lifecycle and named physical DB stores with an
  optional object layer.

`forge_db_object` must not import `forge_rocksdb`, plugins, app lifecycle or
product code.

## Object Declaration

Objects derive from `forge::db::object::object<Derived, Space, Type>`. The base
owns `id` as `forge::db::ids::typed_id<Space, Type>` and contains no storage
behavior.

```cpp
struct account : forge::db::object::object<account, 1, 7> {
   std::string name;
   std::uint64_t balance = 0;
   std::uint32_t region = 0;
};

BOOST_DESCRIBE_STRUCT(account, (forge::db::object::object<account, 1, 7>), (name, balance, region))

struct by_id;
struct by_name;
struct by_region_balance;

using account_object = forge::db::object::object_index<
   account,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<
         by_name,
         forge::db::object::member<&account::name>>,
      forge::db::object::ordered_non_unique<
         by_region_balance,
         forge::db::object::composite_key<
            forge::db::object::member<&account::region>,
            forge::db::object::descending<
               forge::db::object::member<&account::balance>>>>>>;

FORGE_DB_OBJECT(account_object)
```

`object_index<T, indexed_by<...>>` is a schema descriptor, not a base class.
User values remain described C++ structs. `FORGE_DB_OBJECT(...)` specializes
`forge::db::object::index_for_id` with the compile-time mapping from
`typed_id<Space, Type>` to the descriptor, so typed-id operations do not require
spelling the object type again. The macro declares no `forge::db::ids` symbols and
does not perform runtime registration.

Ordered descriptors follow the Boost.MultiIndex separation between index kind
and key extraction. `member`, `const_mem_fun` and `global_fun` extract scalar
keys; `composite_key` combines two or more extractors. Extractors are ascending
unless wrapped in `descending`; `ascending` can make the default explicit.

Index values use `sort_key<T>` to produce canonical ascending bytes. Forge
ships codecs for booleans, integers, enums, strings, object IDs, typed IDs and
fixed-byte values exposing `to_uint8_span()` or `extract_as_byte_array()`.
Products can support strong domain types without changing DB Object:

```cpp
template <>
struct forge::db::object::sort_key<domain_float> {
   forge::db::object::sort_key_bytes operator()(const domain_float& value) const;
};
```

The specialization owns normalization and invalid-value policy. For example, a
SoftFloat consumer decides how signed zero and NaN values behave. Codec failures
surface as `forge::db::object::exceptions::invalid_index_key` before index or
object records are mutated.

## Store

`forge::db::object::store` receives a shared `forge::db::core::driver` and a DB Object
record family:

```cpp
auto driver = std::make_shared<forge::db::rocksdb::driver>(
   forge::db::rocksdb::config{
      .path = "data/rocksdb",
      .families = {"objectdb"}
   });

auto store = co_await forge::db::object::store::open(
   driver,
   forge::db::object::store::config{
      .family = forge::db::core::family{"objectdb"}
   });

store.register_object<account_object>();
```

`forge.db.object.registration` provides immutable **native** application-model
metadata when the model owner and the store composition live in different
translation units:

```cpp
import forge.db.object.registration;

// Implement this catalog function in the translation unit owning the model.
forge::db::object::registration account_registration() {
   return forge::db::object::registration::of<account_object>();
}

// Composition receives the catalog result without importing the account model.
store.register_object(account_registration());
```

The factory is constrained to `application_object_model`; it retains the same
native `object_id_of<Object>` and `std::type_index{typeid(Object)}` as the existing
template overload. `type()` and `model()` return values. There is no default or
untyped constructor, mutable metadata, callback, reflection or serialization
contract. This value belongs to the current native program and is not a wire or
persisted schema descriptor. It does not register system objects.

Both registration forms share the original engine and reject duplicate IDs,
including a different native model with the same ID, as `invalid_descriptor`.
They do not change object bytes, allocators, transactions or index descriptors.

`open()` validates or creates the persisted DB Object header before returning.
It rejects non-empty families without a header and versions outside the
supported range. The cached header is available without I/O through
`store.header()`.

`store::open_existing(driver, config, options)` is the noninitializing alternative.
It reads the actual family/header through a Core snapshot and reuses the same
header decoder and validation. The header must already exist at the current
format version: missing/malformed headers report `invalid_header`, and a
non-current/unsupported version reports `incompatible_version`. It never creates
or upgrades a header, reserves an ID, begins a write transaction, commits or
invokes application mutation callbacks. A driver without snapshot reads is
rejected; there is no creating/write fallback. The returned store is not
read-only: subsequent ordinary transactions and writes retain their normal rules.
This validates the layer header, not application consistency or pristine state.
Native driver open/lock/filesystem activity is outside this logical-record promise.

## System Objects

Object space `0` is reserved for Forge system objects. Application objects must
use a non-zero space. The built-in `forge::db::object::header` is stored at
`{space=0,type=0,instance=0}` and records the DB Object persisted-format
version.

System objects are ordinary typed models for reads and indexes:

```cpp
import forge.db.object.header;

const auto cached = store.header();
const auto persisted = co_await store.get(forge::db::object::header_id);
```

They derive from `system_object<Derived, Type>`. Public `insert`, `create`,
`replace`, `modify` and `erase` accept only application objects, so attempts to
mutate the header are rejected at compile time. Bootstrap and future migration
code use a private path that does not invoke application interceptors or
observers.

`forge.db.object.system` owns the central catalog of reserved system type IDs
and the infrastructure-only access path used by DB family libraries. DB
Revision uses this path for its state, entry and delta models. Those rows remain
readable through normal Object reads and indexes but cannot be mutated through
application write APIs.

The default write policy is `single_writer`, which serializes DB Object
mutations at the store layer. `write_policy::backend` is available for drivers
that intentionally own write concurrency. The writer and allocator lanes use
the neutral FIFO `forge::asio::gate`; cancellation while waiting is reported as
`forge::asio::exceptions::canceled` and never transfers ownership of a ticket.

Generated IDs use `id_allocation_policy::monotonic` by default: rollback and
savepoint rollback leave intentional gaps. Consensus stores that require the
same logical transition to produce the same object IDs can opt into
`id_allocation_policy::transactional`. That policy requires `single_writer`;
its sequence records participate in the active transaction and DB Revision, so
rollback, savepoint rollback and revision revert restore both objects and their
next IDs.

## Transactions And Direct Calls

All mutations use transaction semantics. Direct `store` calls open a short
transaction, delegate to the same mutation pipeline, commit on success and
rollback on failure:

```cpp
co_await store.insert(account{...});
auto created = co_await store.create<account>([](account& value) {
   value.name = "alice";
   value.balance = 100;
});
co_await store.replace(account{...});
co_await store.modify(account::id_t{42}, [](account& value) {
   value.balance += 100;
});
co_await store.erase(account::id_t{42});
```

`insert(value)` is strict and expects `value.id` to be already assigned. Use
`create<T>(...)` when DB Object should allocate a new ID. Generated IDs are
monotonic per `{space,type}`, start at instance `0`, and are not allocated again
after erase, rollback or failed insert; gaps are expected.

Use an explicit transaction when several object changes must commit together:

```cpp
auto tx = co_await store.begin_transaction();
auto created = co_await tx.create<account>([](account& value) {
   value.name = "bob";
});
co_await tx.modify(account::id_t{42}, [](account& value) {
   value.region = 3;
});
co_await tx.commit();
```

If an uncommitted transaction is destroyed, DB Object asks the underlying
`forge::db::core::transaction` to rollback best-effort. Explicit rollback propagates
backend rollback errors after cleanup.

## Shared Transactions

DB Object can join an existing `forge::db::core::transaction`. This is how DB Object and
DB Blob share one backend commit boundary:

```cpp
auto db_tx = co_await driver->begin_transaction();

auto object_tx = co_await objects.join(db_tx);
auto blob_tx = blobs.join(db_tx);

auto digest = co_await blob_tx.put(bytes);
co_await object_tx.insert(metadata{.digest = digest});

co_await db_tx.commit();
```

`store.begin_transaction()` is the convenience owning path. `store.join(tx)` is
the shared path and does not own commit/rollback. Joining is asynchronous:
`write_policy::single_writer` waits for the store writer lane and keeps it until
the outer Core transaction commits, rolls back or is dropped. Joining an
existing Object transaction from the same store reuses its participant and
returns another non-owning facade; it never acquires a second writer ticket.
`write_policy::backend` delegates serialization to the backend and does not wait
on the Object writer lane. Store instances over the same driver and family share
the same neutral gate, so dropped-transaction cleanup and a reopened store cannot
overlap backend writes.

## Pre-commit Projection

`transaction::projected_changes()` returns the current logical ObjectDB change
set, including only mutations that survived savepoint rollback.
`transaction::add_precommit_observer()` installs a transaction-scoped observer
that runs through DB Core's active before-commit phase. The observer receives
the final change set and may perform ordinary reads or writes through already
joined transaction layers. Writes made by an observer become part of the same
ObjectDB participant and physical commit; later observers see those additions.

Observers must be installed before commit starts. Reentrant registration,
commit, rollback or participant attachment is rejected. An observer failure
leaves the outer transaction rollback-only, so callers can perform one explicit
rollback and preserve all joined-layer atomicity.

## Reads And Indexes

Direct reads use `begin_read()` and stable backend snapshots:

```cpp
auto value = co_await store.get(account::id_t{42});
auto maybe = co_await store.find(account::id_t{42});
```

Index queries are Boost.MultiIndex-style and execute through persisted ordered
index records:

```cpp
auto alice = co_await store.index<account_object, by_name>().find("alice");

auto page = co_await store.index<account_object, by_region_balance>()
   .equal_range(std::uint32_t{3})
   .page({.limit = 100});

auto stream = store.index<account_object, by_region_balance>()
   .equal_range(std::tuple{std::uint32_t{3}})
   .stream({.page_size = 100});

auto exact = co_await store.index<account_object, by_region_balance>()
   .find(std::uint32_t{3}, std::uint64_t{100});

auto tail = store.index<account_object, by_region_balance>()
   .lower_bound(std::uint32_t{3}, std::uint64_t{100});
```

`find` requires a complete composite key. `equal_range`, `lower_bound` and
`upper_bound` accept a non-empty ordered prefix. Variadic and tuple forms use
the same key encoder. Streams keep one read snapshot for their whole lifecycle.

## Ranked Indexes And Aggregates

Ranked descriptors persist ordinal spans and aggregate totals beside the
ordinary index records. `member` accepts a chain of member pointers, so a sum
projection can reach nested fields without a product-specific extractor:

```cpp
struct by_id;
struct by_state;
struct by_payload_bytes;

using upload_object = forge::db::object::object_index<
   upload,
   forge::db::object::indexed_by<
      forge::db::object::ranked_primary_unique<
         by_id, forge::db::object::ranked_schema<1>>,
      forge::db::object::ranked_non_unique<
         by_state,
         forge::db::object::member<&upload::state>,
         forge::db::object::ranked_schema<1>,
         forge::db::object::sum<
            by_payload_bytes,
            forge::db::object::member<&upload::payload, &payload_ref::size>>>>>;
```

`ranked_unique` is the corresponding unique secondary descriptor. Every ranked
index supports global and range aggregates plus the full ordinal surface:

```cpp
auto index = store.index<upload_object, by_state>();

const auto total = co_await index.count();
const auto bytes = co_await index.sum<by_payload_bytes>();
const auto item = co_await index.nth(10);
const auto position = co_await index.rank(*item);

const auto lower = co_await index.lower_bound_rank(active);
const auto upper = co_await index.upper_bound_rank(active);
const auto [first, last] = co_await index.equal_range_rank(active);

const auto active_count = co_await index.equal_range(active).count();
const auto active_bytes =
   co_await index.equal_range(active).sum<by_payload_bytes>();
```

`find_rank(key)` returns the first exact-match rank or the index size.
`range_rank(lower, upper)` and `range(lower, upper)` use `[lower, upper)`.
`nth(n)` returns `nullopt` outside the index. `rank(object)` checks the exact
persisted entry and calculates its position through one Core snapshot or
transaction, including the Object ID for unique indexes, and reports typed
`not_found` for stale values.

The persisted engine is a deterministic augmented skip-list. Root totals make
global `count` and `sum` O(1); spans make `nth`, rank, bounds and range
aggregates expected O(log N). Only `nth` loads an Object record. Aggregate and
rank queries read index metadata and never scan or materialize all objects.

Ranked records are updated in the same Core transaction as the Object and its
ordinary index entries. Savepoints, rollback, snapshots and DB Revision follow
the same state boundary. Arithmetic is checked before the first mutation and
throws `aggregate_overflow` instead of wrapping.

The default `single_writer` policy uses the Object writer lane. With
`write_policy::backend`, a ranked mutation requires backend record locks and
the Object participant claims one private coordinator record for its physical
family. Core acquires all participant coordinator claims in canonical
`(family, key)` order before the transaction's first read-for-update or
mutation; ranked roots are then locked in descriptor order. This deliberately
serializes backend-policy Object writers per family, including across processes,
and prevents opposite model or family mutation order from deadlocking. A
backend without `record_locks` rejects the mutation before changing Object
state.

Every ranked descriptor requires `ranked_schema<Version>`. The root stores and
exactly compares a canonical schema descriptor containing Object type, index
ordinal and kind, key-layout version, explicit schema version, and ordered sum
slot accumulator representations. Increase `Version` whenever the extractor,
ordering, index kind, sum projection, accumulator or slot order changes.
Opening an empty type creates state on its first mutation. If Object rows
already exist but ranked state is absent, reads and writes fail closed with
`aggregate_rebuild_required`; Forge never performs a hidden startup scan.
Incompatible or corrupt root/span data reports `aggregate_corruption`. Any
ranked schema change on populated storage therefore requires an explicit
migration or rebuild outside this API.

An Object store can also join a Core snapshot opened by the same driver:

```cpp
auto read = co_await driver->begin_read();
auto objects = object_store.join(read);

auto account = co_await objects.get(account::id_t{42});
auto by_name = co_await objects.index<account_object, by_name>().find("alice");
```

The joined view reuses the existing Object decoder, schema registration and
index implementation. It does not open a second backend snapshot. Closed,
origin-less and foreign-driver snapshots are rejected with typed DB Object
exceptions.

## Hooks

Interceptors run before mutation writes and may veto. Observers run after a
successful commit with a `change_set`; they are not called for rollback or
failed commit. Hooks are DB Object-level and do not expose backend write batches.

## Modules

- `forge.db.object.object`: base object and descriptor mapping.
- `forge.db.object.header`: persisted format header and its system descriptor.
- `forge.db.object.system`: reserved system type catalog and infrastructure
  registration/access contract.
- `forge.db.object.index`: index declarations, views, range queries and streams.
- `forge.db.object.cursor`: DB Object pagination validation over
  `forge.db.core.record` request types.
- `forge.db.object.transaction`: high-level DB Object transaction.
- `forge.db.object.snapshot`: read-only snapshot view.
- `forge.db.object.hooks`: interceptors, observers and change sets.
- `forge.db.object.store`: store, direct wrappers and write policy.
- `forge.db.object.exceptions`: typed `forge.db.object` errors.
- `<forge/db/object/macros.hpp>`: macro-only typed-id mapping declaration.

Deterministic key layout and record materialization are private implementation
details.

## Index Roadmap

DB Object currently supports primary unique, ordered unique and ordered
non-unique indexes, including composite keys and mixed ascending/descending
components. Hashed indexes are deferred: persisted ordered backends already
provide exact and range access, while a backend-neutral hash contract needs a
separate demonstrated use case. Boost.MultiIndex `sequenced` and
`random_access` indexes do not map to this persisted object/index model. Ranked
indexes require a separate backend-neutral rank-maintenance design.

## Migration Groundwork

Runtime migration/catalog support is intentionally out of this block. These are
migration events for a future explicit migration layer:

- changing `{space, type}`;
- changing index order or index kind;
- changing an extractor or composite-key member order;
- changing base object serialization;
- changing the ordered key codec.

The candidate transaction-integrated revision journal and its boundary with a
future migration runner are documented in
[`docs/iterations/forge-db-revisions-migrations-v1.md`](../../../docs/iterations/forge-db-revisions-migrations-v1.md).

## Transactional allocator projection

Normal store-owned transactional allocations also publish typed
`change_set::allocations` entries with exact sequence before/after values.
Missing sequence is logical zero; explicit inserted IDs can be occupied while
next is zero. Erasing rows never reconstructs or lowers next from live IDs.
Counter-only changes (including an initializer that throws after allocation)
participate in precommit observers, stale-projection checks and savepoints.

`transaction::allocation_next<Model>()` and
`snapshot::allocation_next<Model>()` read the existing sequence without
initializing or reserving an ID. They use the same private codec as allocation.

A transaction's operations/lifecycle must be serialized by its owner, including
aliases and direct Core commit/rollback/savepoint calls. The participant rejects
overlapping allocator calls before their first suspension; this is not a queue
or a general concurrent-transaction API. Commit/savepoint preparation rejects an
in-flight allocation. Monotonic high-watermark/seal semantics remain unchanged
and are not a reversible consensus-counter contract. Custom allocator
constructors keep their API; their arbitrary allocation source is not exposed
as a store-owned sequence trace. `transaction::tracks_allocations()` reports
this actual store-owned transactional single-writer capability (false for custom
allocators, monotonic/backend policies and finalized transactions).
`store::runtime_options()` exposes the immutable options actually selected by
the owner; the existing Store plugin object handle forwards this query without
re-reading configuration or caching a second policy.

The added allocations vector changes the C++ layout of public change_set.
Rebuild ObjectDB and its binary/module consumers with the normal CMake/package
graph; do not mix old objects/BMIs. This does not introduce a Raw archive format
or change application/contract row bytes.

## Pristine provisioning precondition

`co_await objects.require_pristine(object_tx)` accepts only the current Object
header and end of the whole owning family. It rejects every other persisted
record, including an allocator sequence whose value is zero, indexes and unknown
system records. Header bytes must equal the canonical Raw encoding of the
current owner header; no second decoder is introduced. No record is initialized,
normalized, erased or written.

Pass an active transaction obtained from THIS store's `begin_transaction()` or
`join(core_tx)`, including aliases from `join(object_tx)`. The actual policy
must be `single_writer`; monotonic IDs are allowed for non-consensus provisioning.
Foreign/manual/closed transactions and backend-writer policy fail with typed
errors before the scan. The same Object participant retains its writer lease
through the enclosing Core transaction's commit/rollback, even if a local alias
is destroyed. Serialize all transaction operations and perform provisioning in
that SAME transaction; a stale snapshot or a separately opened transaction is
not a check-to-write guarantee. This proves current namespace contents, not
freshness of a signing key, another DB family, a filesystem path or erased history.

Revision has a separate `revisions.require_pristine(object_tx)` predicate for
its exact initial infrastructure. Do not use the header-only Object predicate
to accept a family that already contains Revision metadata.

The infrastructure-only `system::access` bridge has fixed header-only and
header+Revision-state0 shapes, not a caller-supplied record allowlist. Its Revision
entrypoint returns that exact payload for the mandatory owning decoder/default
state check. Product code uses the concrete Object or Revision store predicate.

## Restoring exact allocation state into a fresh namespace

`store::restore_allocations(object_tx, std::vector<store::allocation>)` writes a
bounded batch of exact next-ID values using the existing private sequence codec.
Each entry contains a canonical application type ID (instance zero) and its
`next` value. Entries must be strictly sorted and unique, refer to registered
application types, and cannot outnumber the owner's registered model set.
The product owns its closed manifest and must reject missing or extra models.

The SAME active Object transaction and actual owner/driver must use
`single_writer` plus `transactional` allocation. The complete Object family must
contain ONLY its canonical header, with no row, index, sequence (even zero),
Revision state or unknown record. The existing writer lease and pristine scanner
enforce this before the first counter write. A store with existing or previously
allocated state cannot use this operation to lower its counters.

Zero is canonical absent sequence; it does not create a persisted zero record.
`UINT64_MAX` is a valid exhausted next value, and subsequent ordinary `create`
fails with the existing typed exhausted-sequence error. Explicit row insertion
still does not advance next. Restoring next from `max(row IDs) + 1`, dummy
creates or a raw sequence-key write is not equivalent. Consumers separately
validate row-ID/reference invariants against authenticated counter values.

All encoding and trace capacity reservation happen before writes. The SAME
participant publishes each successful `0 -> next` trace, including logical
zero, so normal observers, stale-projection checks and successful savepoint
rollback retain their ordinary behavior. Operations/aliases/lifecycle must stay
serialized. Allocation admission exists before lazy coroutine execution;
dropping an unstarted operation denies commit until full rollback.

Any error after admission, including cancellation or a pristine-check failure,
is sticky until FULL transaction rollback. Catching the error or rolling back a
savepoint does not make a partial restore committable. Pre-admission manifest,
policy or encoding errors perform no writes. No ordinary allocator, monotonic
seal, persisted row format or global default changes.

Importers that need Revision infrastructure use an isolated unpublished stage
without Revision for row/counter/authenticated-graph import, then strictly close
it and reopen normally with Revision before writing local recovery metadata.
This API does not restore a journal or activate a store. Do not attach a Revision
capture to the import transaction. Existing Blob/Authenticated participants must
be attached before their first mutation; their ownership/validation contracts
remain unchanged.
