# forge_db_blob

`forge_db_blob` is a neutral content-addressed blob store over a shared
`forge::db::core::driver`.

It owns blob mechanisms: put/get/has/stat/erase/verify, explicit
retain/release/ref counts and bounded collection of unreferenced blobs. It does
not own runtime GC scheduling, disk-pressure policy, encryption, erasure coding,
manifests or product namespaces.

## Logging

The private `forge_log` dependency uses `forge.db.blob`. DEBUG lifecycle and
autonomous `blob.committed` / `collection.committed` events run after the
owning transaction has completed. Collection reports only removed/limit
counts, including empty passes; it never emits routine INFO. Payloads, digests,
owners, keys and native exception strings are omitted. Joined transactions do
not invent separate Blob commit ownership or duplicate backend outcomes.

## Store

```cpp
auto driver = std::make_shared<forge::db::rocksdb::driver>(...);

forge::db::blob::store blobs{
   driver,
   forge::db::blob::store::config{
      .data_family = forge::db::core::family{"blobdb.data"},
      .refs_family = forge::db::core::family{"blobdb.refs"}
   }};
```

The default `put(bytes)` path uses `forge::crypto::digest::sha256` and returns
`forge::db::blob::ref<forge::db::blob::digest>`:

```cpp
auto content = co_await blobs.put(payload);
// content.digest == forge::db::blob::hash<forge::db::blob::digest>{}(payload)
// content.size   == payload.size()
```

Alternative digest algorithms do not require a templated store. Provide
`forge::db::blob::hash<Digest>` and `forge::db::blob::digest_traits<Digest>`, then
use the thin typed wrappers:

```cpp
auto content = co_await blobs.put_as<product_digest>(payload);
co_await blobs.verify(content);
```

## References

`forge::db::blob::ref<Digest>` is the typed value used by metadata layers and APIs
when they need to store both the content digest and the blob size:

```cpp
forge::db::blob::ref<forge::db::blob::digest> value{
   .digest = forge::db::blob::hash<forge::db::blob::digest>{}(payload),
   .size = payload.size()
};
```

`forge::db::blob::digest` is the default digest type and aliases
`forge::crypto::digest::sha256`. Other digest types can participate by specializing `forge::db::blob::hash<Digest>` and
`forge::db::blob::digest_traits<Digest>` with hashing, byte conversion, text
conversion and a stable algorithm id.

Variant/JSON-friendly conversion is a string in the form `<digest-text>:<size>`,
for example `<sha256-hex>:1234`. Binary raw serialization stays compact and
typed: fixed-size digests are encoded as digest bytes followed by the `uint64`
size; variable-size digests add a binary `uint32` digest length before the digest
bytes. It does not serialize through the text form.

DB Blob private keys include the digest algorithm id and digest bytes, so two
algorithms that produce the same bytes do not collide in data or ref records.

## Shared Transactions

DB Blob can join any active `forge::db::core::transaction`, including one also used by
DB Object:

```cpp
auto tx = co_await driver->begin_transaction();
auto object_tx = co_await objects.join(tx);
auto blob_tx = blobs.join(tx);

auto digest = co_await blob_tx.put(bytes);
co_await blob_tx.retain(digest, owner);
co_await object_tx.insert(metadata{.digest = digest});

co_await tx.commit();
```

Joined DB Blob transactions do not own commit/rollback. Standalone
`blobs.begin_transaction()` remains the convenience path.

If multiple components need Blob access through the same Core transaction,
reuse the first Blob transaction instead of attaching a second participant:

```cpp
auto first = blobs.join(tx);
auto second = blobs.join(first);
```

Both facades share the existing Blob participant and remain non-owning. Calling
`blobs.join(tx)` again is intentionally rejected as a duplicate raw Core join;
passing a Blob transaction from another store is rejected as well.

## Read Snapshots

`forge.db.blob.snapshot` provides a read-only view over one Core snapshot:

```cpp
auto read = co_await driver->begin_read();
auto view = blobs.join(read);

auto payload = co_await view.get(content);
auto owners = co_await view.ref_count(content);
```

`store::begin_read()` is the standalone convenience path. The snapshot exposes
`get`, `has`, `stat_blob`, `verify` and `ref_count`; mutation, collection and
commit operations are intentionally absent. Joining validates that the Core
snapshot is active and belongs to the same driver. Payload size/digest checks
are identical to transaction reads.

Copies share the native snapshot and may be read concurrently when the backend
advertises snapshot support. Long-lived snapshots retain old record versions
and may retain RocksDB SST and Blob files, so callers should scope them to one
operation or a bounded batch.

## Retention

`retain`, `release`, `ref_count` and `collect_unreferenced(limit)` are library
mechanisms. Runtime policy such as when to collect, what owners are live, how
much to delete per pass and how to report metrics belongs to plugins or
products.

Object-backed metadata can use its canonical Forge ID directly as the owner:

```cpp
co_await blobs.retain(content, forge::db::blob::owner_ref{metadata.id});
co_await blobs.release(content, forge::db::blob::owner_ref{metadata.id});
```

Both `forge::db::ids::object_id` and `forge::db::ids::typed_id` are supported. Their
owner identity is exactly `forge::raw::pack(object_id)`, with no textual,
prefix or version bytes. This preserves byte compatibility with callers that
already construct the same binary owner explicitly. Custom string or binary
owner schemes remain responsible for using identities distinct from Object
IDs.

When a DB Revision participant is attached, owner-ref changes remain
reversible. Removing an owner ref creates an internal retention barrier when a
future revert may need the payload. Barriers are not included in public
`ref_count()`, but the collector honors them. Revert or prune removes the
barrier atomically with the corresponding revision history.

Payload puts are excluded from revision history. Payload erase and explicit
collection are forbidden inside an active revision scope, because physically
removing content required by a before-image would make revert incomplete.
Collection is rejected before scanning, including zero-limit or no-op calls.
Automatic garbage collection remains outside the library.
