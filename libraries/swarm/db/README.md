# forge_swarm_db

Declarative ObjectDB metadata for immutable Swarm content. Use this leaf when
an application needs indexed catalog/job metadata and one transactional restart
checkpoint alongside the canonical descriptor. Do not use DB rows for payload
bytes, received network blocks, remote protocol messages or proof of availability.

The C++ models and metadata schema are **Preview**, schema version **1**.
Persisted compatibility is separate from source compatibility: changes to field
order, object IDs, index order/kind or key extraction require an explicit schema
version and migration/reset decision, never a silent reinterpretation.

## Target And Modules

Target `forge_swarm_db` (installed `Forge::forge_swarm_db`), package component
`swarm_db`. Dependencies are `forge_swarm_core`, `forge_db_object` and
`Boost::headers`. No concrete database driver or Swarm file store is selected.
The STATIC target owns the two real module interface units; it has no dummy
implementation source or import-only facade.

- `forge.swarm.db.content`: `content`, `job_state`, `content_index` and query tags.
- `forge.swarm.db.schema`: `schema_state`, `schema_index`, `schema_version` and
  `schema_id`. It does not import the content model.

## Metadata V1

Object space **81** is reserved for these models. `schema_state` is type **1**,
with the sole permitted instance `schema_id{0}` and `uint32_t version = 1`.
Singleton/version enforcement belongs to the consuming adapter, not an import
side effect. `content` is type **2** with a monotonically allocated ObjectDB ID.

Content Raw field order after the inherited `uint64_t id.instance` is:

| Field | Type And Meaning |
| --- | --- |
| `swarm` | Core `swarm_id`, the 32-byte descriptor identity |
| `manifest` | Core canonical `descriptor`, with its existing version/bounds |
| `locator` | Local storage locator string, not part of content identity |
| `state` | `job_state`, defaults to `paused` |
| `priority` | Signed `int32_t`, higher numbers first within one state |
| `expires_at` | Signed `int64_t` Unix seconds; `INT64_MAX` means no expiry |
| `checkpoint` | Optional Core `resume_state`, containing its own generation |

`job_state` has explicit values `paused=0`, `downloading=1`, `seeding=2`,
`failed=3` and a `uint8_t` C++ underlying type. Native described Raw follows its
existing **int64** enum representation; this model adds no enum-wire override.
Serialization is the existing Raw/reflection contract, not a second row codec.
The optional checkpoint is a Core value, not a second copy of the checksummed
sidecar envelope. Core descriptor/checkpoint versions remain independent of the
catalog schema version.

All content metadata and its checkpoint occupy one row. ObjectDB updates that
row and its indexes in one transaction; there are no per-block database rows.

Index ordinals and key order are part of metadata v1:

| Ordinal / Tag | Kind | Key |
| --- | --- | --- |
| 0 / `by_id` | Primary unique | Object ID |
| 1 / `by_swarm` | Ordered unique | `swarm.value`, raw digest bytes, not hex |
| 2 / `by_locator` | Ordered unique | Exact locator string |
| 3 / `by_state_priority` | Ordered non-unique | State ascending, priority descending, swarm bytes ascending |
| 4 / `by_expiry` | Ordered non-unique | Expiry ascending, swarm bytes ascending |

Nested `member` extraction reuses DB Object's digest sorting; no hash-to-hex
allocation or custom key encoder is introduced. The unique swarm index also
makes the final composite-key tie breaker deterministic.

## Explicit Composition And Bounded Queries

The following registers descriptors only. The application supplies an already
opened Object Store and owns its lifecycle. It must establish/check the schema
in a transaction before admitting ordinary catalog operations.

```cpp
#include <boost/asio/awaitable.hpp>

import forge.swarm.db.content;
import forge.swarm.db.schema;
import forge.db.object.index;
import forge.db.object.store;

namespace catalog_models = forge::swarm::db;

void register_models(forge::db::object::store& objects) {
   objects.register_object<catalog_models::schema_index>();
   objects.register_object<catalog_models::content_index>();
}

boost::asio::awaitable<forge::db::object::object_page<catalog_models::content>>
next_downloads(forge::db::object::store& objects) {
   co_return co_await objects.index<catalog_models::content_index,
                                   catalog_models::by_state_priority>()
      .equal_range(catalog_models::job_state::downloading)
      .page({.limit = 16});
}
```

Continue via the returned ObjectDB cursor rather than collecting the full
catalog. Exact identity lookup passes `swarm_id.value` to `by_swarm`; expiry
selection uses a bounded composite-key range, not a filtered full-history scan.

## Validation, Authority And Tests

These are value declarations, not a validated persistence API. A consuming
adapter must validate descriptor identity, checkpoint identity/geometry, state,
locator bounds and expiry before accepting a row. A locator is neither a safe
filesystem path nor permission to open it. Preserve UTF-8 names; filesystem
mapping and normalization/collision policy belong to the file adapter.

Select exactly one checkpoint authority for an operation: sidecar or ObjectDB.
A catalog row alone never establishes verified file bytes. Payload verification
and filesystem durability precede checkpoint commit; DB transactions do not
make payload and metadata one atomic resource. Erasing a row does not erase
payload. Locators and descriptors may reveal local paths and filenames: do not
log or expose whole rows to remote peers.

A failed durable flush or after-commit observer can leave committed metadata;
an exception is not proof of rollback. A checkpoint consumer must restore the
driver's durable flush fence before treating a loaded generation as durable.
Ordinary metadata lookup makes no payload-availability or durability claim.

Acceptance for the composing storage adapter covers identity/locator uniqueness,
state/priority/expiry cursor queries, exact schema/version handling, rollback,
reopen, checkpoint generation/handle lifetime and durability failures through
the existing Forge drivers. Model import performs no registration, reads,
driver open, task scheduling or runtime lifecycle work.

See [DB Object](../../db/object/README.md), [Core](../core/README.md) and the
[Swarm plan](../../../docs/iterations/forge-content-swarm-v1.md#73-indexed-metadata-in-swarm-db).
