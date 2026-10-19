# forge_swarm_conversion

Explicit, validated mappings between local Swarm catalog metadata and the public
immutable metadata record. Use this optional leaf at a local composition
boundary, never as a transport dependency or a blind database save adapter.
Public source API: **Preview**. This leaf defines no new wire/storage format.

Target `forge_swarm_conversion`, installed `Forge::forge_swarm_conversion`,
package component `swarm_conversion`. Dependencies: `forge_core`, `forge_swarm_db`
and `forge_swarm_protocol`. It selects no DB driver and performs no I/O,
registration, transaction, task scheduling or runtime lifecycle operation.
Protocol/API consumers do not need this leaf and must not link DB just to
exchange immutable metadata.

## Real Specializations

Module `forge.swarm.conversion.conversion` exports explicit specializations of
the primary template from `forge.core.conversion` in its owning C++ namespace:

```cpp
#include <utility>
import forge.swarm.conversion.conversion;

namespace core = forge::swarm::core;
namespace db = forge::swarm::db;
namespace protocol = forge::swarm::protocol;

protocol::metadata export_metadata(const db::content& row) {
   return forge::core::conversion<protocol::metadata>{row}.finish();
}

core::descriptor import_descriptor(protocol::metadata remote) {
   return forge::core::conversion<core::descriptor>{std::move(remote)}.finish();
}
```

Constructors validate before retaining a result. `finish() &&` returns an owned
value and consumes that builder once; a repeated finish is a typed
`protocol::exceptions::invalid_metadata` error. It does not retain the source
row by reference. All nontrivial methods are defined in `conversion.cpp`.

Export validates the canonical descriptor identity, valid job-state membership,
nonnegative expiry, a nonempty 1..4096-byte non-NUL local locator, and any Core
checkpoint's identity/geometry/publication relationships. It uses Core's existing
validation, not a second checkpoint schema. Backend crypto/allocation failures
are not silently swallowed. Object IDs are opaque local values; all valid local
IDs produce the same public metadata for the same immutable identity.

Export deliberately does **not** require downloaded/published content. A valid
partial checkpoint, paused/failed job or missing checkpoint may still supply
immutable metadata. Even a seeding job label is not independent proof of present,
durable or verified payload. The public value contains exactly `swarm` and
`manifest`: ID, locator, state, priority, expiry and checkpoint are omitted, not
redacted into a different record. Descriptors can reveal logical filenames;
neither metadata nor a local locator should be logged blindly.

Import validates canonical descriptor identity and returns **only** the
descriptor. There is no `conversion<db::content>`, remote row constructor or
trusted/local checkpoint mapping. The owning application must separately assign
its own local ID, safe locator, paused job settings and trust/admission decision
inside an explicit transaction. Metadata identity is not authorization and does
not certify the sender or local payload availability.

Costs are canonical descriptor/hash validation, optional checkpoint validation
and one bounded descriptor copy for export. Import takes an owned metadata value
and moves its descriptor after validation. Core's metadata/geometry limits apply,
but nested string/vector overhead is not an exact total-memory budget. Callers
bound concurrency and run synchronous hashing/validation on their own bounded
compute execution; no hidden thread pool is created here.

Tests in `tests/swarm_conversion` cover private-field omission, partial metadata
export, invalid identities/rows/checkpoints, import validation, compile-time
absence of the inverse DB mapping, and one-shot builders. Build, integration and
installed-package acceptance remain separate coordinator checks.
