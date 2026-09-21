# forge_chain_protocol

## Contract commitments

forge.chain.protocol.contract_commitment owns the canonical contract-table
authenticated-state layout used by verified table reads and change streams. It
provides contract_table_location, the exact `table_value { payer, uint32_t
count }` metadata record, primary and secondary value records, family
identifiers and deterministic key derivation. These bytes are a persisted
consensus commitment: consumers must use the supplied helpers rather than
reconstructing metadata, prefixes or secondary keys manually.

`forge_chain_protocol` provides canonical blockchain protocol values and wire
records. Use it to build, serialize, hash and sign Forge-compatible actions,
transactions and blocks. It builds on `forge_chain_core` without adding
controller, execution or node runtime behavior.

The flat `forge.chain.protocol.audit`, `info`, `block_query`, `state_query`,
`transaction_query` and `admin` modules own the
transport-neutral chain wire contract. API classes, clients and transport
bindings remain in `forge_chain_api`; wire DTOs never live beside those runtime
interfaces.
Table-scope pagination carries opaque authenticated keys as `bytes` in
`table_scope_request::cursor` and `table_scope_response::next`; callers must
not parse or reconstruct them from contract scope names.
Typed table/account change pagination likewise carries only opaque `bytes`
cursors. Raw authenticated-tree point, range, key and mutation records are not
part of this protocol library.

Package component: `chain_protocol`. Public namespace:
`forge::chain::protocol`.

## Modules

- `forge.chain.protocol.types`: digest aliases, names, assets, symbols, IDs,
  keys, signatures, timestamps and scalar wire vocabulary.
- `forge.chain.protocol.fixed_key`: `fixed_key<Size>` and `key256`, with
  ordered word construction, canonical raw bytes and fixed-width hex variants.
- `forge.chain.protocol.native_ids`: canonical native object-ID aliases backed
  directly by `forge.db.ids.typed_id`. These named IDs are the stable public
  identity of native protocol records; they do not expose generic ObjectDB
  access and this library intentionally provides no `get_object` operation.
- `forge.chain.protocol.entity_selector`: exact entity selection by one native
  ID or one natural key; it carries no lookup behavior.
- `forge.chain.protocol.account`, `account_metadata`, `permission_usage`,
  `permission`, `full_permission`, `permission_link`, `account_authority` and
  `full_account`: flat canonical account and authority projections. Persisted
  primitives retain explicit native IDs and Spine field order; composed views
  use inheritance without depending on ObjectDB records.
- `forge.chain.protocol.code` and `table`: canonical native code and contract
  table projections with stable named IDs. `code_hash` is also the digest of
  the persisted code blob, so the public code projection carries only its size
  and never duplicates that digest in a second blob-reference field.
- `forge.chain.protocol.currency_stats`: the donor-compatible token statistics
  row in exact `supply`, `max_supply`, `issuer` order.
- `forge.chain.protocol.generated_transaction`: host projection of persisted
  deferred-transaction metadata plus `packed_transaction`. The canonical
  mapping from the persisted blob uses no signatures, `compression::none`,
  empty context-free data and the donor transaction bytes in `packed_trx`;
  construction remains the responsibility of a future builder.
- `forge.chain.protocol.resource_limits`, `resource_usage`,
  `resource_limits_config`, `resource_limits_state`,
  `account_ram_correction`, `resource_meter` and `account_resources`: objective
  persisted resource primitives plus transport-neutral account resource views.
  A meter is unlimited when both `max` and `available` are absent and bounded
  when both are present. Mixed presence is invalid; `valid(resource_meter
  const&)` reports this invariant for later API validators. Meter fields are
  supplied data and this library performs no recovery or limit policy.
- `forge.chain.protocol.chain_config`, `wasm_parameters`, `ratio`,
  `elastic_limit_parameters`, `usage_accumulator`,
  `activated_protocol_feature`: canonical persisted and guest state values with
  Raw and host Variant contracts, but no controller policy. The activated value
  remains exactly `feature_digest` plus `activation_block_num` on the Raw and
  guest ABI surfaces.
- `forge.chain.protocol.protocol_feature`,
  `activated_protocol_feature_info`: host API values for complete protocol
  feature metadata. The shared base carries digest, description, dependencies,
  type and specification; activated API information adds ordinal and block
  number. Subjective restrictions remain exclusive to
  `supported_protocol_feature`.
- `forge.chain.protocol.float64`, `float128`: bit-preserving floating values;
  their host ordered-key helpers reject NaN and use the canonical Spine byte
  order.
- `forge.chain.protocol.authority`: permission weights and authority thresholds.
- `forge.chain.protocol.producer_schedule`: legacy producer keys and schedules
  used by block headers and contract APIs.
- `forge.chain.protocol.producer_authority`: weighted block-signing authorities
  and producer authority schedules.
- `forge.chain.protocol.producer_info`: canonical `eosio.system` producer row.
  Its optional block-signing authority is a trailing binary extension: omission
  contributes no Raw bytes, while a present authority uses its ordinary
  `[index, payload]` wire representation without an optional presence marker.
- `forge.chain.protocol.finalizer_vote_record`: typed finalizer vote diagnostics
  with the canonical BLS public key, block ID and block timestamp values.
- `forge.chain.protocol.finalizer_authority`: import-compatible alias that
  preserves the public type name while forwarding ownership to the canonical
  typed `forge.chain.savanna.values` finalizer record. Its `public_key` field
  intentionally follows the approved pre-stable migration to the fixed BLS
  value type.
- `forge.chain.protocol.finalizer_policy`: generation-free Spring contract
  payload that reuses the canonical finalizer record. It is distinct from the
  generation-bearing operational Savanna policy.
- `forge.chain.protocol.action`: actions and the Savanna action digest that
  commits to the return value.
- `forge.chain.protocol.action_receipt`: canonical action receipts, witness
  hashes and Savanna receipt digests.
- `forge.chain.protocol.transaction`: transactions, packed transactions,
  compression, IDs and signing digests. It re-exports the action module.
- `forge.chain.protocol.transaction_trace`: typed transaction and action
  execution traces shared by runtimes, block metadata and Chain API clients.
- `forge.chain.protocol.block`: block headers, receipts, signed blocks, block
  IDs and transaction receipt Merkle roots.
- `forge.chain.protocol.abi`: ABI records and optional tail-field decoding.
- `forge.chain.protocol.system`: canonical system action payloads.
- `forge.chain.protocol.audit`, `info`, `block_query`, `state_query`,
  `transaction_query`, `admin`: common audit envelopes and
  endpoint wire records for the transport-neutral Chain API.
  Typed table/account change feeds expose ordered per-block batches with a
  `state_anchor` on every batch; product protocol does not expose raw proof
  keys, point/range records or authenticated change-range DTOs.

The target publicly links `forge_db_ids`, `forge_exceptions`, `forge_chain_core`,
`forge_compression`, `forge_raw`, `forge_variant`,
`forge_crypto_asymmetric_values`, `forge_crypto_asymmetric` and
`forge_crypto_digest`.

Authority, producer schedule, producer authority and finalizer policy modules
share guest-safe value partitions with host wrappers. Host wrappers provide the
same full Raw and Variant serialization contract; `block_signing_authority`
uses the Spring/FC `[index, payload]` JSON representation.

Code, table and currency-stat projection values use only guest-safe protocol
scalars and Raw mechanics. The generated-transaction projection is intentionally
host-only because `packed_transaction` is not part of the guest value surface.
Producer info and finalizer vote records remain host-only projections; they are
not registered as Contract SDK guest modules.

The `finalizer_authority` leaf remains as an import-compatible forwarding
module only. It does not own a second type or serialization implementation.

`forge::chain::protocol::digest` aliases `forge::chain::core::digest`. Protocol
names such as `chain_id`, `block_id`, `transaction_id` and `checksum256` remain
in the protocol namespace while sharing the same SHA-256 representation.

## Fixed-Size Keys

```cpp
#include <cstdint>

import forge.chain.protocol.fixed_key;

const auto key =
   forge::chain::protocol::key256::make_from_word_sequence<std::uint64_t>(
      0, 0, 0, 42);
const auto bytes = key.extract_as_byte_array();
```

Raw serialization uses the canonical fixed-width byte sequence. Variant
conversion uses fixed-width hexadecimal text. Ordering follows the stored word
representation and is covered by compatibility fixtures.

## Transactions And Blocks

```cpp
#include <chrono>
#include <deque>

import forge.chain.protocol.block;

auto transaction = forge::chain::protocol::transaction{};
transaction.expiration = std::chrono::sys_seconds{};

auto receipts = std::deque<forge::chain::protocol::transaction_receipt>{};
auto header = forge::chain::protocol::block_header{};
header.transaction_mroot =
   forge::chain::protocol::calculate_transaction_mroot(receipts);
```

Protocol declarations preserve field order and canonical raw encoding.
Transaction IDs, signing digests, block IDs and receipt digests must be computed
with the provided helpers over canonical raw representations.

## Savanna Action Receipts

```cpp
#include <array>

import forge.chain.core.merkle;
import forge.chain.protocol.action_receipt;

auto receipt = forge::chain::protocol::action_receipt{};
receipt.receiver = executed_action.account;
receipt.act_digest = forge::chain::protocol::generate_action_digest(
   executed_action, return_value);

const auto leaf = forge::chain::protocol::calculate_savanna_action_digest(
   receipt, executed_action);
const auto action_root = forge::chain::core::calculate_merkle_root(
   std::array{leaf});
```

The Blockchain execution runtime owns sequence allocation and receipt
construction. Forge owns the receipt wire record and deterministic hashing.
The Merkle root above is the root of executed action receipt digests used by
Savanna finality data. In a proper Savanna block, the historical
`block_header.action_mroot` field carries a finality-tree root claim instead;
the two roots must not be substituted for one another.

## Producer Reward Projection

`forge.chain.protocol.producer_rewards` defines a canonical, evidence-oriented
producer reward view with two fixed sources. `system` is the `eosio`
`producers` source: its `active`, `unpaid_blocks`, `last_claim_time` and first
strictly later `next_claim_time` describe the producer-row inputs to the
system claim. `eligible` is the row-and-anchor-time predicate
`active && anchor_time >= next_claim_time`, not a promise about global system
state. Its `claim_action` is exactly
`eosio::claimrewards { owner }`. `bpay.claimable` is the exact `quantity` from
the `eosio.bpay` `rewards` row scoped by `eosio.bpay`, keyed by the producer
account. Its `bpay_reward` source layout is exactly `{ owner, quantity }` and
its `claim_action` is exactly `eosio.bpay::claimrewards { owner }`. Both fixed
actions reuse the canonical `claimrewards { owner }` payload; the action
account distinguishes their contract semantics.

`project_producer_reward(producer_info, optional<bpay_reward>, anchor_time)`
is the public pure construction point for both sources. API responses carry an
`anchor_header`; a verifier hashes it back to the finalized anchor before
passing its timestamp to that projector. The projector returns an empty
optional for malformed rows or an unrepresentable next-claim time; absence of
the `bpay` row is valid and still yields a projection.

When `eosio.bpay::claimrewards` has erased that row, the projection preserves
that fact as an empty optional; it never substitutes an `eosio.token` liquid
balance or invents a zero asset.

The system source deliberately does not calculate a producer reward amount or
promise that its action will succeed: global activation, execution time and
dynamic reward buckets are evaluated by `eosio.system`. Neither source
constructs a transaction; callers supply transaction headers, authorization
and signatures elsewhere.

## Boundaries And Safety

Text and variant conversion are API and diagnostic representations, not signing
preimages. Parse untrusted keys, signatures, compressed transactions and raw
payloads through the typed Forge codecs and handle their typed failures. Private
key custody and signing authorization belong to the product runtime.

This library does not provide state storage, controller behavior, transaction
execution, action trace collection, sequence allocation, consensus, finality,
P2P synchronization, block production, Merkle proofs or finality-tree
construction. Account/resource projections are public protocol snapshots, not
ObjectDB models, persisted indexes or calculation services.
Their defaults intentionally mirror the current Spine donor state. Generic
described Variant decoding is permissive and preserves declared defaults when
fields are absent; exact API readers are responsible for validating requests.

## Tests

`test_forge_chain_protocol` covers raw and variant fixtures, fixed keys, names
and assets, native state projections, producer binary-extension compatibility,
typed finalizer votes, transactions, compression, signatures, ABI compatibility,
block IDs, transaction receipt roots and Spring-compatible Savanna action receipts.
`test_forge_package_chain_protocol_component` verifies the installed
`chain_protocol` component, protocol imports and the transitive core digest
dependency.
