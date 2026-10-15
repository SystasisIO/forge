# Immutable Secret Versions v1

Date: 2026-10-15. Status: **design gate approved; stage 1 locally verified, integration pending**.

This document specifies an additive, local-only versioned secret capability for
`forge_plugins_crypto_secrets`. The interfaces below are implemented in the
feature worktree. The inspected base is
`8975f234dabf2cbfb91d766bf32194f3d8832a5b` (`origin/dev` at worktree creation).
The [donor note](../donors/forge-secret-versions-v1.md) records current owners.

## Scope And Compatibility

The plugin gains immutable material versions, explicit-version operations,
safe metadata, durable create/revoke, compare-and-swap and durable replay.
Products continue to own caller authorization and the meaning of purpose names.
There is no remote vault, new key-management service, transport, automatic
rotation, product policy, or default/latest-version read.

The existing `api` contract id/version, virtual methods, request/result records,
`config`, enum values, reflection field order, and `value`/`file`/`encrypted_file`
sources remain unchanged. Configured secrets and managed versions have separate
lookup domains: the old API never starts resolving managed entries, and the new
API never silently falls back to configured entries. A managed id may equal a
configured id without ambiguity because the contract and explicit version select
the domain. There are no compatibility aliases or copied crypto implementations.

Existing `FORGEKS1` encrypted-container bytes and signing-keystore payloads stay
unchanged. Managed versions use a new authenticated payload *inside* the same
container, never an existing signing-key store populated with arbitrary bytes.
No existing file is migrated or reinterpreted automatically.

## Local Capability Contracts

All methods return `boost::asio::awaitable<T>`. All three interfaces derive from
`forge::api::core::contract<..., surface::local>` and use `FORGE_EXPORT_API`.
Only the leaf namespace `forge::plugins::crypto::secrets` owns these symbols.

| Interface | Contract id, version | Authority |
| --- | --- | --- |
| `version_write_api` | `forge.plugins.crypto.secrets.version_write`, 1.0 | Create/revoke; no material read |
| `version_metadata_api` | `forge.plugins.crypto.secrets.version_metadata`, 1.0 | Bounded safe descriptions/list/status |
| `version_api` | `forge.plugins.crypto.secrets.version`, 1.0 | Explicit-version purpose-gated material/crypto operations |

Possession of a write/metadata handle does not imply a read capability. The local
registry is a trusted host composition boundary, not a sandbox: a host able to
acquire arbitrary handles is already trusted. A product must selectively expose
its own administrative operations; these contracts cannot be published remotely.

Value records (the reflection block follows declarations):

```cpp
struct version_ref {
   std::string secret_id;
   std::uint64_t version = 0; // positive, never reused for this id
};

enum class version_state { available, revoked };

struct version_policy {
   std::vector<std::string> purposes;
   std::vector<operation> operations;
   bool allow_raw_export = false;
   std::uint64_t max_plaintext_bytes = 0;
   std::uint64_t max_ciphertext_bytes = 0;
   std::uint64_t max_aad_bytes = 0;
};

struct create_version_request {
   std::string mutation_id;
   std::string secret_id;
   std::uint64_t expected_revision = 0;
   secret_kind kind = secret_kind::bytes;
   version_policy policy;
   forge::crypto::core::secret_bytes material; // move-only native RAII material
};

struct revoke_version_request {
   std::string mutation_id;
   version_ref ref;
   std::uint64_t expected_revision = 0;
};

struct version_mutation_result {
   version_ref ref;
   std::uint64_t revision = 0; // revision committed by this mutation
};

struct version_metadata {
   version_ref ref;
   std::uint64_t revision = 0; // current revision of this secret
   secret_kind kind = secret_kind::bytes;
   version_state state = version_state::available;
   version_policy policy;
};

struct version_query { version_ref ref; };
struct version_list_request {
   std::string secret_id;
   std::uint64_t after_version = 0;
   std::uint32_t limit = 32;
};
struct version_page {
   std::vector<version_metadata> versions;
   std::uint64_t next_after_version = 0; // zero when exhausted
};
```

`version_write_api` exposes `create_version(create_version_request)` and
`revoke_version(revoke_version_request)`, both returning
`version_mutation_result`. `version_metadata_api` exposes
`describe_version(version_query) -> version_metadata`,
`list_versions(version_list_request) -> version_page`, and
`status(query) -> version_snapshot`. All three handles are provided even when
the managed store is disabled: status then reports `enabled=false`,
`ready=false` and zero counts; material/metadata mutations and reads fail typed
`store_unavailable`. New work after stop fails `stopped`; safe status remains
available. The new `version_snapshot` contains only
enabled/readiness/stopping/fault flags and aggregate version/receipt counts, no material,
file paths, bootstrap information, digests, receipt fingerprints or last errors.

`version_api` exposes the four crypto/material operation names of the existing `api`, with
new concrete request/result wrappers. Each request wrapper contains
`std::uint64_t version` and the existing operation request as `request`; each
result wrapper contains `std::uint64_t version` and the existing result as
`result`. For example, `version_get_request { version; get_request request; }`
and `version_get_result { version; get_result result; }`. This reuses canonical
operation fields and avoids a second HKDF/AES DTO definition. Version zero is
always invalid. The selected material is exactly `(request.secret_id, version)`.
There is no overload that accepts only a secret id.

The four wrapper pairs are `version_get_request/result`,
`version_derive_request/result`, `version_aead_encrypt_request/result`, and
`version_aead_decrypt_request/result`. Existing request/result reflection does
not change. Sensitive wrappers over existing serializable request/results are
Schema-secret. Native `create_version_request` contains move-only `secret_bytes`
and has a `diagnostic_is_secret` hook for the whole request; it is not a wire or
Schema decode DTO. No serializer for `secret_bytes` is introduced. Their
diagnostics must redact before encoding. Raw bytes are returned only by the explicit trusted
`version_api::get_bytes` escape hatch, never a mutation/metadata result.

This is supported by the current local-contract machinery: `FORGE_EXPORT_API`
selects `EnableRaw=false` for a local-only surface, and the descriptor builds
wire encoders/decoders only under `if constexpr (EnableRaw)`. A local handle
invokes the typed implementation directly; the generated remote proxy path
rejects local-only invocation without instantiating a request codec. The first
implementation tests must compile/register/call a move-only create request.
Native callers transfer already-owned `secret_bytes` into the coroutine frame;
destroying a never-awaited/abandoned awaitable still destroys that RAII owner.
An external input adapter must move input into `secret_bytes` before this call
and owns its earlier transport/decoder buffers. Schema redaction alone is not
claimed to wipe ordinary vectors or buffers owned by a caller.

## Policy, CAS, And Replay

The configured managed-store ceiling lists allowed purposes/operations, raw
export permission, and byte limits. A version policy must be a nonempty subset
of that ceiling. Raw export additionally requires `get_bytes` and
`allow_raw_export=true` in both ceiling and version policy. A mutation cannot
expand configured authority. Per-version zero limits resolve to the configured
ceiling before persistence; all resolved limits are positive and bounded.
Purpose/operation lists are canonical sorted sets; duplicates are rejected.

Configuration is fixed for one plugin lifecycle. On reopen, every available
version's persisted policy must remain a subset of the current configured
ceiling, including byte limits and raw export. A narrower incompatible ceiling
fails startup with a safe typed configuration error and does not modify the
store or open material admission. Revoked versions cannot be read and do not
prevent narrowing. Widening a ceiling never changes any persisted version
policy. There is no silent policy intersection or policy rewrite.

Each secret has a monotonic `revision`, initially zero for an absent id. Every
successful create or first revoke increments it exactly once. Create allocates
the next positive version; revoke does not change material, kind, policy or
version identity. Revocation is irreversible in v1. A new version may be created
after a prior version is revoked, with current CAS and a new mutation id.
Counter overflow is a typed capacity failure before any disk write.

Successful mutations are indexed by caller-supplied `mutation_id` in the
encrypted snapshot. After structural input bounds, and before current-policy,
CAS or capacity rejection, lookup the receipt:

1. Same token and same canonical request returns the original ref and committed
   revision without another write, even if the supplied CAS is now stale.
2. Same token with any changed field, material byte, method or CAS returns
   `idempotency_conflict`.
3. An unknown token must match the current per-secret revision, otherwise
   `revision_conflict`; a failed operation creates no receipt.

Canonical comparison uses HMAC-SHA256 from `forge_crypto_digest`, keyed by a
random 32-byte per-store key generated once and stored only inside the encrypted
payload. Its input is a versioned domain separator, mutation kind, canonical
request metadata, submitted policy/limits (including zero default markers), CAS
and exact material bytes. Do not resolve zero defaults using current config
before computing a replay fingerprint: an identical retry after a config
change still identifies its original receipt. Plaintext
comparison buffers are wiped. The fingerprint/key are never metadata or error
fields. No unkeyed material hash is published. The authenticated container covers
the key, receipts, counters, material and revocation tombstones together.

The receipt contains only mutation id, fingerprint and result; it is encrypted
with the rest of the state. There is no plaintext journal. A replay of a create
whose version is now revoked returns the original reference/revision and does
not revive it. Mutation results deliberately contain no historical `available`
flag; `describe_version` always provides current state. A fresh token attempting a second
revoke is rejected as `version_revoked` after CAS validation.

## Bounded Managed Configuration

The old public `config` record is not extended. A new typed
`version_store_config` is decoded from the additive
`plugins.crypto.secrets.versions` subsection. `describe_config()` composes the
existing `config` descriptor with prefixed descriptors for that subsection;
decoding both sections uses existing typed Schema/Config facilities. Tests must
prove old decoding tolerates the subsection and effective-config/CLI/env
redaction preserves nested secret flags. No parser, environment reader, or
second configuration precedence rule is introduced.

The proposed config record contains `enabled`, `mode`, `path`,
`bootstrap_passphrase_file`, `ceiling` (a `version_policy`), and the finite limit
fields below. Paths are owned strings decoded by Config; the bootstrap file
and store-path fields are Schema-secret in diagnostic descriptors. Policy and
finite capacity settings remain visible as configuration metadata.
The subsection is disabled by default. Enabling it requires an absolute store
path, explicit `open_existing` (default) or `create_new` mode, a bootstrap
passphrase-file path outside the
managed store directory, a policy ceiling and finite limits. Missing/corrupt
existing state is never treated as permission to create. `create_new` is an
explicit first-provisioning mode that refuses an existing file; after that
successful provisioning, the operator selects `open_existing` for later starts.
It is not an open-or-create default. First creation is exclusive and does not
overwrite an existing file. The bootstrap file contains
one nonempty passphrase, at most 64 KiB; one trailing LF/CRLF may be removed, but
other whitespace remains significant. Reading reuses
`forge::crypto::keystore::read_password`'s file-source behavior. The
passphrase is not resolved through
managed ids, put in a version, persisted alongside the container, or accepted
through any mutation. Missing/wrong bootstrap never permits plaintext fallback.

| Limit | Proposed default / v1 hard ceiling |
| --- | --- |
| Distinct secret ids | 256 / 1,024 |
| Total versions, including revoked | 1,024 / 4,096 |
| Versions per id | 32 / 256 |
| Material per version | 64 KiB / 1 MiB |
| Total retained material | 4 MiB / 32 MiB |
| Serialized store plaintext | 8 MiB / 64 MiB |
| Mutation receipts | twice total-version limit; never evicted |
| Id / mutation id | 256 / 128 bytes, fixed maxima |
| Purposes per policy / purpose bytes | 32 / 128, fixed maxima |
| Metadata page | 32 default, 128 maximum |
| Pending managed calls / retained input bytes | 16 / 1 MiB defaults; 64 / 8 MiB hard ceilings |
| Simultaneous store mutations / admitted crypto operations | 1 / 2 |
| scrypt | existing default 16,384/8/1 and 32 MiB; configured ceilings validated before allocation |

The receipt bound must be at least twice the total-version bound: each version
can create at most one create receipt and one revoke receipt. Failed/replayed
operations consume none. This reserves the ability to revoke every retained
available version even when creation capacity is exhausted. Creation must also
reserve **serialized bytes** for every still-available version's future revoke
receipt. Before accepting a create, require
`encoded_candidate_size + available_versions * max_encoded_revoke_receipt_size`
plus worst-case collection-prefix growth to fit the plaintext payload limit.
The schema-specific reserve uses the hard maximum mutation-id/id lengths,
fixed fingerprint/result sizes and maximum counter encodings, not average
sizes; its v1 upper bound is 1 KiB per available version. Revoke consumes its
precharged reserve. Loaded state is checked against the same invariant.
Thus byte-full creation cannot prevent a first revoke with maximum valid token
length. Version/receipt
history is never silently evicted. No delete/compaction API is required in v1;
creation fails typed when full. Operational migration to another store is a
future explicit operation, never an automatic reset.

Byte reservations are acquired before moving input into queued work; allocation
and input-size checks precede scheduling. Count and byte exhaustion return typed
`busy` immediately, with no unbounded coroutine queue retaining secrets. The
compute pool's own bounded queue remains an additional limit. Existing AES/HKDF
limits still apply to version operations; HKDF salt/info/output are bounded too.
Store parsing enforces payload, collection, string, nesting and total-allocation
limits before allocation, not only after decoding. Reuse
`forge::raw::unpack_exact(..., unpack_limits)` with storage-specific
`max_container_elements`, `max_total_container_elements`, `max_bytes` and
`first_container_elements`; do not use the default million-element allowance.
Per-byte allocation is capped at the largest allowed material/id/receipt field,
aggregate elements at the computed schema maximum, and input at the payload
limit. The schema has fixed nesting, and parsing rejects inconsistent per-field
counts before publication. A bounded canonical re-encoding must exactly match
the decrypted input before live-resource conversion; this rejects narrowed enum
integers and noncanonical encodings without changing the legacy Raw codec.
Encoding first uses the existing zero-buffer `datastream<std::size_t>` counter,
then an exact-size wiped buffer and fixed-pointer stream, with equal final
lengths required. No Raw wire or public API extension is introduced.

## Storage And Durable Acknowledgement

The store is one bounded encrypted snapshot. A read uses published in-memory
material and never rewrites or decrypts the file. Ownership verification may
inspect the stable lock through the bounded compute worker before admission;
it never performs filesystem work on the caller's network executor. Only
successful create/revoke or
initial empty-store creation encrypts/writes a snapshot. Whole-store rewrite is
an explicit bounded mutation cost; it is not a per-read hot path.

The new payload begins with an explicit managed-store schema id/version, then
the receipt key, per-secret counters, immutable versions/policies/material,
revoked state and receipts in canonical order. It is not the signing-store entry
vector. A schema golden fixture fixes this new persisted contract. Unsupported
schema versions, duplicate ids/versions/receipts, inconsistent counters,
references, fingerprints or policy violations fail closed.

Use `forge.crypto.keystore.ownership` for exclusive process ownership and its
single active `claim()`. The stable sibling `.lock` file is never replaced or
deleted. Lock/process/inode verification, private directory and owner-only file
requirements remain shared with the existing keystore. Forked or competing
owners are rejected. Relative paths and client-controlled file paths are not
accepted by the managed APIs.

Extract the existing private-directory preparation, bounded owner-only read,
and atomic write from `crypto/keystore/store_impl.cpp` into the same leaf's new
`forge.crypto.keystore.persistence` module. Its synchronous functions are
`prepare_private_directory(path)`, `read_container(owner, max_bytes)` and
`write_container(owner, bytes_span, replace)`. They use the existing ownership
object, not another lock protocol. Both signing store and managed version store
use that implementation. Preserve first-create no-overwrite, short I/O/EINTR
handling, 0700 directories, 0600 files, no-follow checks, directory-chain sync,
temporary cleanup and the existing pre/post-publication exception boundaries.
The refactor must not modify crypto bytes or existing store API behavior.

A mutation is serialized by the store owner, outside lifecycle admission locks:

```text
admit + reserve
  -> replay lookup / CAS / policy / capacity
  -> prepare immutable candidate, receipt and response (all fallible allocations)
  -> Raw encode + existing keystore::encrypt_file
  -> exclusive 0600 same-directory temporary file
  -> complete write + fsync(file) + close
  -> atomic rename (exclusive link for initial creation)
  -> fsync(parent directory)
  -> non-throwing publish of candidate
  -> acknowledge ref + committed revision
```

Successful acknowledgement means complete encrypted state and its directory
entry passed the supported filesystem durability fence, and the in-memory
authority reflects that state. Read admission and candidate publication share a
short store admission mutex; no file/KDF/crypto work or await runs under it.
Mutation serialization may span I/O on a compute worker, but never blocks a
network executor or the read admission mutex.

After rename, failure is **not** an ordinary rollback. `durability_unknown`
publishes/retains the prepared candidate and fences every new managed read/write;
status reports fault without exception text. The store retains ownership.
Restart/reopen validates the authoritative stable file before reopening
admission. No API returns a false old state. Pre-publication failures preserve
the previous authority and return a safe typed failure. Ownership loss also
fences admission. All required response/snapshot allocations happen before
publication; publication/error fencing itself is non-throwing.

## Recovery Matrix

| Crash/failure point | Recovery and response |
| --- | --- |
| Before temporary creation, during write/encrypt, before rename | Stable old snapshot remains authoritative; no success; bounded recognized encrypted temporaries may be removed after ownership acquisition |
| After temp fsync, before rename | Never promote the temp; stable old snapshot wins |
| After rename, before directory fsync / before acknowledgement | No success promised; disk may recover old or new snapshot, both internally atomic; retry the *same* mutation id and exact request |
| After durable fence, before response | Stable new snapshot contains receipt; exact retry returns original result |
| After acknowledged create or revoke | New state/receipt survives a normal crash/restart on the supported filesystem |
| Stable file truncated, corrupt, wrong password, unsupported schema | Initialization fails closed; no backup rollback, temp promotion, reset or plaintext fallback |
| Missing bootstrap or unavailable crypto | Initialization fails closed; retained encrypted bytes are not replaced |
| Ownership lost or post-rename durability error | Fence new operations; preserve ownership/candidate as applicable; explicit restart/reopen required |

Stale-temp cleanup uses a dedicated private store directory and explicit
budgets for **all examined directory entries**, total inspected name bytes and
cleanup operations, including names that do not match its store-specific
temporary prefix. It refuses unexpected matching links/types and never touches
the stable lock or bootstrap file. Reaching any scan/cleanup budget fails
initialization for explicit operator cleanup; it does not enumerate an
unbounded directory first. No recovery path
logs file contents, passphrases or decoded exceptions from crypto/Raw.

This is local crash durability on a supported local filesystem. It does not
prevent a privileged attacker deleting/rolling back the whole disk, protect a
lost bootstrap passphrase, provide hardware key custody, or guarantee physical
erasure from memory, swap, SSD history or backups.

## Snapshot, Revoke, Lifecycle, And Execution

Material uses `forge.crypto.core.secret_bytes` from the native request boundary;
bootstrap uses `secret_string`.
Maps hold immutable records and shared material resources. A new operation
selects exactly one record and checks availability, purpose, operation, sizes
and raw-export permission at one admission point. It retains that resource,
not a copied entire material map. Candidate maps shallow-share unchanged
resources; serialization/transient buffers are bounded and wiped on every exit.

Revoke prepares/persists a replacement metadata record, then publishes it at
the same admission point. Calls admitted before that point may finish with their
retained resource; calls admitted afterward fail `version_revoked`. Revocation
does not withdraw bytes already returned or erase an active caller's copy.
No resurrection, cancellation of arbitrary C++ crypto, or hard-erase claim is made.
Revoked versions remain counted and retained under the explicit bounded policy.

Managed initialization, recovery, file access, scrypt, serialization/HMAC and
versioned crypto execute through application-owned `context.compute()`.
Enabling the managed store without a bounded compute executor is a typed
configuration failure. Do not create a plugin-local pool, `std::async`, polling
thread or additional runtime. The wallet provider's admitted-work gate and
compute admission are the donor. The shared App shutdown wrapper supplies the
separate `co_spawn` child with a disconnected cancellation slot: managed waits
retain caller cancellation masks/history and restore its throw policy. Do not
copy a cancellation-state reset into the caller's native coroutine.

Common Forge lifecycle owns admission. A managed mutation reserves an owner
before I/O; once accepted, it completes commit or failure reconciliation even
if stop/caller cancellation races with rename. Do not reapply `try_admit` after
durable publication and discard the result. `request_stop()` synchronously
closes new admissions; `shutdown()` joins accepted initialization/mutations/crypto
before clearing owned state and releasing the ownership claim. No await, user
callback or destructor runs under the lifecycle admission lock. The shared
application compute pool outlives plugin draining.

The old configured API currently loads during `configure()` before
`plugin_context` exists and executes crypto inline. Its runtime admission rejects
the `created` and `configured` phases and must not be weakened: configured-only
or configure-plus-provide is not an operational-readiness contract. Callers and
test fixtures must run the normal initialize/startup lifecycle before material
operations. The coordinated baseline exposed old fixtures that skipped those
steps; correcting those fixtures is separate from adding new managed behavior.
The strict compute guarantee here covers all **new managed** work; changing the
old configured execution model is a separate scheduling decision, not a hidden
side effect.

New typed failures are additive enum values without renumbering old values:
invalid version/request, revision conflict, idempotency conflict, revoked,
store unavailable, busy/capacity, ownership lost and durability unknown.
Error context uses fixed operation/category names and persisted public secret ids;
denied caller purposes are removed at the managed boundary. It includes no material, passphrase,
raw input, HMAC, mutation token, encoded request, resource path or nested crypto
exception message. Existing purpose/operation denial categories remain effective.

## Test-First Implementation Gates

The design gate is approved. Each behavior first gets a failing
focused test, run and observed under the coordinator-approved build budget.

1. Preserve the current configured-source/API tests and package-consumer smoke;
   add raw golden coverage for old DTO field order and a positive old config
   with the new disabled subsection. Prove all three new contracts are local;
   compile/register/call move-only create input, reject remote publication, and
   cover abandoned/canceled native call ownership and diagnostic redaction.
2. First create, exact-version read, safe metadata, raw-export denial, purpose
   and operation denial, policy-ceiling violation, immutable old-version read
   after a second create, zero/missing version, and bounded pagination.
3. Two concurrent same-CAS creates: exactly one success; exact replay before
   stale CAS; changed material/policy/method/CAS with same token conflicts;
   restart replay; identical zero-limit request replay after widening the
   configured ceiling; narrowing below an available version's persisted policy
   fails startup without modifying disk; revoked-only policy permits narrowing;
   replay of revoked create never revives the version.
4. Revoke vs newly admitted/retained reads; acknowledged revoke survives
   restart; stop/cancel at each mutation phase never publishes old authority
   after disk publication; shutdown waits for the admitted owner.
5. Fault injection before temporary write/fsync/close/rename, and after rename
   before directory fsync/response. Check stable contents, receipt recovery,
   typed unknown durability and admission fence. Test-only I/O hooks belong to
   test utilities, never a public production option.
6. Existing keystore regression/golden/ownership tests after persistence
   extraction; two-process lock, replaced lock, fork, private-mode, symlink,
   hardlink, competing first create, EINTR/short I/O, leftover-temp bounds.
7. Corrupt/truncated/authentication-failed/oversized snapshot, hostile Raw
   lengths/counts, malformed schema/counters, missing/wrong bootstrap and
   exhausted scrypt budgets all fail closed without creating a replacement.
8. Count/byte/queue/version/receipt limits, revoke count and byte reservation
   (near-full payload still revokes with the longest token), bounded cleanup
   with many nonmatching entries, no
   secret sentinel in stable-file bytes, recognized temp artifacts, metadata,
   diagnostics, schema output, exceptions or logs. Prove public metadata never
   contains receipt key/fingerprint or secret length/value.
9. Installed consumer imports/obtains new slices and performs create/read/revoke
   using the installed module set; old descriptor consumer still compiles.

Planned focused targets are `test_forge_crypto_secrets`,
`test_forge_crypto_keystore`, and `test_forge_package_plugins_crypto_secrets`,
plus the repository structure/package gates selected by the coordinator. Exact
target availability and baseline results must be recorded from the current
configuration; no test/build success is claimed by this document.

## File Ownership

Design-only changes: this file and the linked donor note.

Stage 1 runtime changes:

- Existing secrets `plugin.cpp`, `plugin_impl.cpp`, `api_impl.cpp`, `config.cpp`,
  paired private headers, `plugin.cppm`, `exceptions.cppm`, `CMakeLists.txt` and
  README: compose lifecycle, config and new capability implementations. Existing
  `api.cppm` and old `types.cppm` require no serialized-layout change.
- New public slices under secrets' existing public include directory:
  `version_types.cppm`, `version_api.cppm`, `version_write_api.cppm`,
  `version_metadata_api.cppm`; declarations/value records and public Schema
  metadata. Schema rule bodies follow the existing `types.cppm` specialization
  pattern; runtime implementation remains in `.cpp` files.
- New paired private components in secrets: `details/version_store.hxx` +
  `version_store.cpp`, `version_store_operations.cpp`, `version_store_rules.cpp`
  and the private owned-call template aspect `details/version_store_owned.hxx`;
  `details/version_catalog.hxx` for its immutable state
  records; `details/version_payload.hxx` + `version_payload.cpp` for bounded
  canonical serialization; `details/secret_operations.hxx` +
  `secret_operations.cpp` for shared existing crypto semantics; each concrete
  API owner in `details/version_api_impl.hxx` + `version_api_impl.cpp`,
  `details/version_write_api_impl.hxx` + `version_write_api_impl.cpp`, and
  `details/version_metadata_api_impl.hxx` + `version_metadata_api_impl.cpp`.
  The owning classes are `plugin::version_api_impl`,
  `plugin::version_write_api_impl` and `plugin::version_metadata_api_impl`.
- Keystore `include/forge/crypto/keystore/persistence.cppm` +
  `persistence.cpp`, existing `store_impl.cpp`, CMake and README: shared
  persistence extraction, with existing crypto format untouched. The existing
  descriptor RAII helper is shared as `details/unique_descriptor.hxx` +
  `unique_descriptor.cpp`.
- `tests/plugins/crypto/secrets_versions_tests.cpp`, existing
  `tests/plugins/crypto/secrets_tests.cpp`, keystore tests, installed consumer
  and `tests/CMakeLists.txt`: behavior, fault and compatibility gates.

## Gate And Evidence

The coordinator approved contract/config/persistence boundaries, finite limits,
the documented legacy scheduling boundary, and native move-only create material
after independent design review. The separately recorded unchanged-source
baseline released the source freeze before test-first runtime implementation.
Legacy test fixtures now perform initialize/startup; runtime admission was not
weakened to accommodate their earlier configure-only behavior.

Stage 1 is implemented in the isolated feature worktree. Candidate 10 passed
the coordinated native build and all five focused test targets:

| Test group | Cases | Passed assertions |
| --- | ---: | ---: |
| Configured secrets | 23 | 74 |
| Signing keystore | 15 | 62 |
| Managed versions | 34 | 788 |
| Wallet regression | 22 | 195 |
| Wallet fault regression | 1 | 7 |
| Total | 95 | 1126 |

There were zero failed, skipped or aborted cases. Source/scope/diff hashes
matched before and after the coordinated run. Coverage includes authenticated
noncanonical enum rejection, inherited managed-handle PID rejection, reserved
revoke capacity, cancellation/stop after rename, and caller cancellation-state
preservation.

The standalone `BUILD_TREE` package consumer also passed configure, build and
execution (all exit codes zero). Through the enabled application lifecycle and
exported modules it performed create, explicit-version read, safe metadata,
revoke and reopen, retaining the durable revocation after restart.

Installed SDK consumption and full downstream application acceptance remain
separate gates after publishing and pinning the new immutable Forge revision;
neither is claimed by this local evidence. This implementation does not modify
vendor/main checkouts, publish commits, or run CI.
