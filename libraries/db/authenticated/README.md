# forge_db_authenticated

`forge_db_authenticated` is Forge's persistent authenticated ordered-state
library. It creates immutable path-copied AVL nodes, content-addressed values,
version roots and independently verifiable point proofs over a DB Core driver.

## Stability

The C++ API, proof schema and persisted node format are **Experimental** until
the external cryptographic review required by the first production consumer is
complete. A release that changes any of those contracts must identify the
affected schema version and require an explicit database reset or migration.

## Package

- CMake target: `forge_db_authenticated`
- package component: `db_authenticated`
- namespace: `forge::db::authenticated`
- modules: `forge.db.authenticated.types`, `.hash`, `.proof`, `.codec`,
  `.standards`, `.store`, `.transaction` and `.exceptions`

The library depends on DB Core and Forge SHA-256. It does not depend on a
concrete DB driver, ObjectDB, a blockchain protocol or an API transport.

## Transactions

Join the authenticated participant before the first mutation or savepoint:

```cpp
auto db_tx = co_await driver->begin_transaction();
auto authenticated_tx = co_await authenticated.join(db_tx, block_number);

const auto preview = co_await authenticated_tx.preview(changes);
co_await authenticated_tx.stage(changes, preview.commitment.state_root);
co_await db_tx.commit();
```

`preview()` performs no writes. `stage()` writes immutable nodes, values and the
new version root through the caller's normal DB Core transaction. Revision
capture therefore sees the authenticated records and restores the previous root
during a reorg. The participant rejects commit if no version was staged.

A product that projects ObjectDB changes registers a transaction-scoped
`forge::db::object::precommit_observer`. The observer receives the final ObjectDB
change set after savepoint rollback and must verify that it matches the staged
authenticated mutation digest. Projection policy remains downstream because
Forge does not know product table or index semantics.

## Retained Checkpoint Export And Fresh Restore

The pending checkpoint API exports BOTH retained state and historical changes
graphs, including full values and tombstones, without rebuilding AVL topology.
Frame format 1 is distinct from hash schema 3 and the persisted record format.
`encode(checkpoint_record)` and `decode_checkpoint_record` reuse the existing
bounded node codec. There is no Spring snapshot-format compatibility claim.

`export_checkpoint(snapshot, exact_root, sink, limits)` retains the SAME copied
snapshot and shared Store/driver owner across callbacks. All other data layers
must read that same snapshot; independently opening a current read view is not
historical materialization. The sink receives left-first postorder state records,
then changes records. A missing/pruned version fails; there is no latest fallback.

`join_restore(core_tx, exact_root, limits)` is synchronous and attaches the SAME
pending Authenticated participant before writes, locks or savepoints. It requires
a driver-opened, active, same-origin transaction, no Revision capture, and an empty
dedicated Authenticated family. Attach other participants before loading data.
`restore_checkpoint(source, verify)` becomes committable only after both graphs,
all hashes/values/topology/counts, source EOF, the mandatory trusted verifier and
owner-built root/reference metadata succeed. The verifier must bind the complete
logical ObjectDB/Blob/counter projection and the REAL outer file EOF/anchor policy;
a self-consistent graph does not authenticate unrelated product records.

Any error/cancellation leaves a sticky pending/failed participant. Catching it,
dropping/never awaiting the operation or handle, trying ordinary stage/preview or
savepoints, or replacing the participant cannot commit a partial baseline. Whole
Core rollback and a new transaction are required. `restored()` reports the exact
completed root; `staged()` is not populated with a synthetic mutation digest.
Core lifecycle and writes remain serialized by the caller; raw Core bypass and
uncooperative callbacks are not supported restoration operations.

Limits are explicit and nonzero. The owner checks record/count/byte arithmetic,
depth, digest-history and iterative traversal storage, plus conservative input/DTO/
encoding/backend-copy transients before allocating traversal storage or calling
the source. `max_working_bytes` covers bounded record/owner data, not process RSS,
standard allocator bookkeeping or unrelated callback-owned retention. File sources
must bound a frame before allocation; a callback cannot allocate arbitrary memory
and make it the library's budget. Existing Core get returns whole local records;
this API does not promise bounded pre-allocation for a corrupted backend record.

Only exact C exists initially: earliest/latest are C, below-C reads/proofs report
version unavailable, and normal C+1 mutations and reference/GC pruning continue.
This neutral component does not implement product trust selection, activation,
finality-key custody, controller import/reorg, transport or an application runtime.

## Reads And Proofs

`get()` and `scan_range()` read a retained immutable version while validating
content-addressed nodes and values locally. `scan_range()` returns the same
ordered ranks, continuation key and optional values as a verified range proof,
but does not construct or serialize witness nodes. It is the normal trusted
server path when an API caller did not request an audit proof.

`range_request::reverse` selects the same half-open `[lower, upper)` key range
from its upper boundary. Results are returned in descending key order. Its
continuation is the exclusive upper boundary for the next page, namely the last
key returned by the current page; forward scans continue from the first omitted
key as their inclusive lower boundary. The direction is part of the serialized
request embedded in the proof, so a verifier cannot be tricked into accepting a
forward witness as a reverse page.

`prove()` and `prove_range()` additionally construct transferable witnesses.
For range proofs, `proof_tree::state` covers the complete version state and
`proof_tree::changes` covers that version's canonical last-write-wins mutation
set, including tombstones. Callers should not generate either proof unless a
consumer explicitly requests audit material.

## Hash Schema

Schema v3 uses SHA-256 with explicit domain and length framing. Inner nodes
authenticate their full ordered interval in addition to the separator:

```text
value = H(value-domain | length | value)
leaf  = H(leaf-domain | tree-domain | key | value-hash)
inner = H(inner-domain | tree-domain | height | size | min-key | max-key |
          separator | left-hash | right-hash)
```

The canonical tree domain is `role-tag | base-domain`, where the leading byte is
`0x01` for state and `0x02` for changes. The role is therefore injective and is
never derived by appending a textual suffix to a caller-controlled domain.

For every expanded inner node, verification requires `min-key = left.min-key`,
`max-key = right.max-key`, `separator = right.min-key` and
`left.max-key < right.min-key`. Height, size and ordered bounds are required for
ranked range proofs; this metadata is consensus-relevant, not a storage cache.

Schema v3 uses persisted node/root format version 4 and is intentionally
incompatible with every earlier experimental format. There is no compatibility
reader or mode; existing experimental stores must be reset or explicitly
migrated before opening them with this version.

## Standards Boundary

`forge.db.authenticated.standards` owns only the dependency-neutral adapter
contract and capability declaration. Forge does not currently ship an official
Cosmos ICS23 protobuf implementation, so `cosmos_ics23_v1` reports no native
codec or verifier. It must be implemented by an adapter backed by the official
`cosmos/ics23` schema and verifier; Forge proof DTOs must not be relabelled as
ICS23.

Official IAVL existence/non-existence vectors and a pinned Go verification
harness live under `tests/db_authenticated/ics23_harness`. They establish the
cross-language conformance boundary but do not claim that Forge hash schema v3
is presently representable by the IAVL `ProofSpec`.

## Boundaries

Range/change multiproofs, pruning and garbage collection are part of the same
library and have native, adversarial and process-crash regression coverage.
They remain Experimental until the external cryptographic review named in the
production activation gate is complete. Legacy IAVL `RangeProof` is not a
supported format. Proof parsing is bounded by explicit key, value, depth, node
and exact serialized-byte limits. Proof depth has an implementation hard cap of
256, independent of caller settings; this is comfortably above the maximum
valid AVL height for a tree whose rank and size are `uint64_t`.

Range generation tracks the exact encoded size as each node is appended,
including varuint count-prefix growth. Before loading another value it first
checks the minimum remaining framing budget, and a fetched value is rejected
before caching or copying when its exact encoded size does not fit. Decoding
binds untrusted collection counts to the remaining payload's minimum canonical
element size before any reserve or growth.

## Benchmark

`benchmark_forge_db_authenticated` is an MDBX-backed executable. The one-million
and ten-million profiles can be registered as opt-in CTest gates with
`FORGE_DB_AUTHENTICATED_ENABLE_1M_PERFORMANCE_TEST` and
`FORGE_DB_AUTHENTICATED_ENABLE_10M_PERFORMANCE_TEST`. They are deliberately not
part of the default unit lane. Both profiles use ordered keys with 32-byte
values:

```sh
cmake --build build/release --target benchmark_forge_db_authenticated -j4
build/release/tests/benchmark_forge_db_authenticated \
   --baseline 1m --path /tmp/forge-authenticated-1m
build/release/tests/benchmark_forge_db_authenticated \
   --baseline 10m --path /tmp/forge-authenticated-10m
```

A short disposable smoke invocation is:

```sh
build/release/tests/benchmark_forge_db_authenticated --keys 10000 --value-bytes 32
```

`--path` must not exist before the run and is preserved afterward. Omitting it
uses and removes a temporary directory. The initial-batch timer covers MDBX
transaction creation, authenticated join/staging and durable commit for every
bounded chunk. It accumulates DB elapsed time per chunk, excluding client-side
mutation construction while never holding more than one chunk. The JSON reports
construction and total load-wall time separately, and the benchmark rejects
overlapping DB/construction intervals. Profile defaults
use 32,768 keys per chunk for `1m` and 65,536 for `10m`, producing 31 and 153
committed versions respectively. These profile-specific chunks bound both peak
staging memory and retained persistent history; custom `--keys` runs retain the
4,096-key default, and `--chunk-keys` always overrides the selected default.

The `initial_batch` metric represents construction of a production-scale
initial state through bounded durable commits. It does not model long-history
retention, block-by-block churn or pruning throughput; those require a separate
workload with an explicit history policy. Point and range timings cover public
proof generation, including snapshot acquisition; proofs omit values so value
size does not make the fixed 256-item range exceed the proof byte limit.

After all timed measurements, the benchmark reads every expected version record,
checks its version/state/change metadata, reads its last committed state key,
confirms that the next chunk's first key is still absent and scans the change
root at the chunk boundary. The reported `retained_versions` count includes only
versions that passed these checks; verification time and content-check count are
reported separately and do not warm or otherwise affect the initial-batch or
proof metrics.

The executable writes one JSON document to stdout. It reports the committed
state/change roots and sizes, initial-batch milliseconds and keys/second, 1,000
point-proof milliseconds/proofs-per-second/average wire bytes, and 100 ranked
range-proof milliseconds/proofs-per-second/average wire bytes/average nodes.
The range limit is fixed at 256. Configuration output also records the workload,
baseline, chunk source, planned version count, MDBX map ceiling and growth step.
Storage output records committed versions, actually verified retained versions,
retention verification work, and the closed MDBX directory's logical byte
footprint and file count.

The hostile-input parser/verifier harness is opt-in and uses Clang libFuzzer,
AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build/fuzz -G Ninja \
   -DFORGE_DB_AUTHENTICATED_ENABLE_FUZZ_TESTS=ON
cmake --build build/fuzz --target forge_db_authenticated_fuzz -j4
build/fuzz/tests/forge_db_authenticated_fuzz -max_total_time=60
```
