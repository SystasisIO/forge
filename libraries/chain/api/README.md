# Forge Chain API

Target and package component: `forge_chain_api` / `chain_api`.

All public modules live directly in `include/forge/chain/api`. Public symbols
live in `forge::chain::api`; `info`, `block`, `state`, `transaction`,
`submission` and `admin` are API contract names, not nested namespaces. There
is no aggregate `forge.chain.api` module.

The library owns transport-neutral chain contracts, HTTP/P2P-capable clients
and proof-verification policy. Wire DTOs live in flat
`forge.chain.protocol.*` modules and the `forge::chain::protocol` namespace.
API methods use existing protocol records and standard containers directly
when they already express the complete request or result.
`forge.chain.api.json_schema` tells the shared OpenAPI generator that canonical
protocol public keys and signatures are JSON strings; it owns no routes or DTOs.

`raw_client` groups ordinary info, block, state and transaction-query handles.
It does not carry the producer-administration contract;
products resolve `admin` separately only in trusted operational processes.
Products also resolve the independent `submission` contract only where
transaction admission is required; neither `raw_client` nor `transaction` can
reach it. `submission_client` owns
that explicit authority and validates only that the remote acknowledgement
names the submitted transaction. That acknowledgement is not a finality claim.
Consumers establish inclusion or finality through
`verified_client::get_transaction_status` or `await_transaction`.

Every transaction submission has a positive bounded `timeout_ms`. Batch
submission uses `transaction_submit_batch_request`, whose `timeout_ms` is the
total deadline for the whole call rather than a per-item multiplier. An owner
captures that deadline once, rejects or stops when it expires, and bounds each
item by the lesser of its own timeout and the remaining batch budget. Sequential
owners must not sum item timeouts or silently continue without a deadline. HTTP
proxies allow a five-second transport grace beyond the declared application
deadline; cancellation remains typed and reaches the active request owner.

HTTP bindings are resource-oriented rather than RPC-shaped. Safe reads with
scalar path/query parameters use `GET`; structured proof ranges, simulations
and mutations use `POST`. Dynamic reads return `Cache-Control: no-store`.
Requests select an optional finalized anchor by block ID, while the complete
chain/root commitment is returned in the audited response context.
Audited requests may also provide `finality_from`, the caller's already trusted
genesis or checkpoint block. It is a proof-construction hint, not server-provided
trust: `verified_client` fills it from the installed verifier and still verifies
the returned witness locally. When a request names a historical block or range,
the client selects the newest retained trust anchor at or before that known
target instead of blindly using the current preferred checkpoint. Recent trusted
checkpoints therefore keep finality witnesses bounded without weakening the
trust bootstrap.
Table-scope continuation uses the transport-neutral protocol `bytes` value;
the HTTP GET binding carries it through the shared JSON query codec and other
transports carry the same bytes without a transport-specific cursor DTO.

Concrete controllers, state schemas, persistence, protocol publication and
network policy remain in downstream products.

## Signing APIs

`forge.chain.api.transaction_signer` is a transport-neutral `1.0` API with
`local | remote` surface. It accepts the canonical
`forge::chain::transaction::unsigned_transaction`, receives an
`forge::api::auth::authenticated_caller` only through trusted server-side
transport injection, and returns the canonical `prepared_transaction`. The API
does not accept caller-provided digests, key identifiers or algorithms and does
not submit the transaction.

`forge.chain.api.block_signer` is a separate `1.0` local/remote contract. Its
`sign(chain::protocol::block_sign_request{chain, header, keys}, authenticated_caller)` returns
signatures in requested public-key order. It signs the canonical
`calculate_block_id(header)`, never an arbitrary digest or the raw header
digest. The HTTP routes are `/v1/signer/sign_transaction` and
`/v1/signer/sign_block`; the same contracts may be published over authenticated
P2P without a signer-owned listener. The required caller argument is supplied
by trusted server dispatch, not by the HTTP body or client.

`forge.chain.api.finality_signer` is a local-only `1.0` API. `identity()`
returns the configured BLS public key and proof of possession, while
`sign_vote()` accepts a canonical Savanna block reference and vote kind and
returns a canonical finalizer vote. Durable safety-state and vote planning stay
with the consensus controller; this API never signs arbitrary bytes.

The implementation owner is `forge.plugins.chain.signer`. Transport plugins
may publish either remote signer API, but the signer plugin opens no sockets and
does not own HTTP or P2P routes.

Each `method_capability` advertises its own HTTP and P2P publication state;
the service never implies that every enabled method is available on every
transport. Typed change methods use the existing `state_changes` audit class;
other typed state projections may use `state_range`. `service_limits`
separately advertises page, state-batch,
transaction-batch, decoded-container allocation, transaction-status scan,
await deadline, request, response, proof and authenticated-state retention
bounds. Transports reject oversized frames, declared containers and HTTP bodies
before allocation or domain decoding. Service owners install
`limited_descriptor<Interface>(limits)` with the implementation; API Core
dispatch and the typed HTTP binding then enforce the same canonical request,
response, proof and item-count limits before and after the implementation call.
Direct in-process calls may use the typed helpers from `forge.chain.api.limits`
when they cross an untrusted boundary. Resource failures use
`forge.chain.api::exceptions::resource_exhausted` across every transport.
Capabilities and limits are operational claims made by the selected peer; they
are not consensus state and are not authenticated by a state proof. Clients use
them for feature negotiation only. `verified_client` and `submission_client`
apply independently configured local limits before dispatch and after receipt,
so a peer cannot weaken client resource policy by advertising larger values.

`verified_client` verifies transport envelopes, chain identity and finality.
Authenticated point/range/change primitives exist only on the local
`audit_verifier` / `projection_verifier` boundary. They are not Chain Protocol
DTOs, API methods or transport routes. A projection verifier recomputes typed
responses from those exact authenticated bytes and owns the concrete state-key
schema. This keeps DB schema knowledge out of the product protocol without
trusting a server-side DTO conversion. Missing projection support fails closed
with `audit_not_supported`; it never falls back to an envelope-only check.
Public client, verifier and authenticated-store boundaries preserve existing
Forge exceptions and translate implementation `std`/Boost failures into their
own typed Forge error contracts. Asio operation cancellation remains a typed
`forge::asio::exceptions::canceled` result rather than an availability error.

## Portable verified client

forge.chain.api.verified_client_factory composes the Forge authenticated-state
verifier with a caller-owned finality verifier. A Savanna product constructs
that verifier from its trusted genesis or checkpoint bootstraps, then supplies
the raw client, chain ID, state domain and projection verifier:

~~~
import forge.chain.api.contract_table_projection_verifier;
import forge.chain.api.savanna_finality_verifier;
import forge.chain.api.verified_client_factory;

auto finality = forge::chain::api::make_savanna_finality_verifier_with_trusts(
    settings.finality_trust,
    settings.additional_finality_trusts);
auto client = forge::chain::api::make_verified_client(
    raw,
    {
        .chain = settings.chain,
        .state_domain = settings.state_domain,
        .finality = finality,
        .projections = forge::chain::api::make_contract_table_projection_verifier(),
    });
~~~

`verified_client` requires a non-null verifier whose declared trusted chain
matches `options.chain` before it can issue a raw request. It never accepts a
bootstrap or selects trust from another options field. The public
`savanna_finality_verifier` verifies each returned witness against a configured
root or one of its 16 newest rolling checkpoints, then atomically promotes a
locally replayed checkpoint only when it descends from the current preferred
anchor. Each rolling checkpoint retains its bounded canonical replay ancestry;
an exact known checkpoint or an older replay can only cache state when its
finalized height and block ID match that retained canonical ancestry. A
competing branch fails as `invalid_finality`, and an older checkpoint beyond
the retained ancestry fails as `trust_required`, without changing trust.
Configured roots remain available, while `trust_anchor_at_or_before()`
selects the newest configured or rolling checkpoint that does not exceed a
known response target. Historical transaction status calls without an explicit
`finality_from` first obtain an unaudited inclusion-height hint, then issue a
separate audited status confirmation using that locally selected retained
anchor; a missing height or unavailable retained anchor fails locally before a
confirmation request, and only the confirmed response is returned or verified.
`preferred_trust()`, `replay_state()` and `verified_replay()` expose the selected
locally trusted state to consumers that need Savanna-specific projection
verification. The latter includes canonical producer opportunities for the
exact finalized anchor and proof, so consumers never reconstruct proposer
policy history themselves. The exact verified replay is retained in the same
bounded parallel-response window, so immediate
replay of an in-flight response does not depend on its source checkpoint still
being retained. Forge verifies the witness locally; it does not load trust
material from files or select product policy. The contract-table projection
verifier covers get_table_rows, get_table_changes and get_producer_rewards,
including canonical key
layout, row/payer bytes, ordering, deletion, cursors and authenticated
continuations. It intentionally does not verify table scope or native product
projections. Those calls fail closed unless a product installs its own
projection verifier.

## Typed block, info and admin reads

`forge.chain.api.block` is contract version `2.2`, `forge.chain.api.info` is
version `2.0`, and `forge.chain.api.admin` is version `2.4`. Major version 2
is a clean break:
there are no compatibility aliases, legacy readers or alternate JSON modes.
Admin revision 1 appends the read-only `get_operator_identity` and
`get_node_status` capability-descriptor methods, mapped to
`/v1/chain/admin/operator-identity` and `/v1/chain/admin/node-status`.
Block revision 2 appends `get_producer_rewards`. Its response has one outer
finality witness and two table sources at that exact finalized anchor:
`eosio`/`producers` and `eosio.bpay`/`eosio.bpay`/`rewards`. The verified
client verifies the outer witness once and each source through the existing
contract-table verifier; it does not trust the returned aggregate. The latter
is the exact donor `rewards_row { owner, quantity }`; a missing row remains an
empty `bpay.claimable` value, never a liquid `eosio.token` balance. The
response carries two fixed canonical action locations: `system` names
`eosio::claimrewards`, while `bpay` names `eosio.bpay::claimrewards`. The
system facts do not promise a claim amount or execution eligibility before
`claimrewards` executes. Neither source contains an unsigned or signed
transaction. Its `anchor_header` hashes to the
finalized anchor before its timestamp feeds the shared pure reward projector;
the server does not construct an independent aggregate.

Admin revision 4 appends trusted local `get_finalizer_status`, mapped to
`/v1/chain/admin/finalizer`. It reports whether the local finalizer is enabled,
whether it belongs to the current policy and its last emitted vote. It is
operational node state, therefore it is intentionally exposed through the
Admin transport only and is not a `verified_client` projection.

Block responses use the complete host `activated_protocol_feature_info` and
reuse the canonical
`chain_config`, `wasm_parameters`, `producer_info` and
`finalizer_vote_record` protocol records. Activated features preserve the
Spring ordinal, activation block, feature and description digests,
dependencies, feature type and specification; subjective restrictions remain
supported-feature administration data only. Producer pagination uses an optional
typed `account_name` lower bound and opaque `bytes` cursor/next values. Present
cursors and continuations must be non-empty and remain bounded by the common
request and response byte limits. Producer response Raw frames each row's
canonical `producer_info` bytes so its trailing authority extension remains
optional without changing the canonical record encoding. Each nested row is
decoded with the parent's configured byte and per-container limits capped by
that row's exact frame size. Nested container allocations consume the same
cumulative budget as audited fields and all other rows. The donor aggregate
vote weight remains canonical `float64 total_vote_weight` between `rows` and
`next` in both Raw and JSON shapes.

`info_response` carries canonical `resource_limits_config` and
`resource_limits_state` projections as `resource_config` and `resource_state`.
The old duplicated virtual/block CPU/NET and total-weight fields are removed.
Admin RAM-correction pages return canonical `account_ram_correction` records.
All of these reads use `GET` and `Cache-Control: no-store`.

## Verified state synchronization

`forge.chain.api.state` is contract version `3.0`. It exposes typed
`get_table_changes` and `get_account_changes` methods; the old remote
`get_point`, `get_range` and `get_changes` surface does not exist. Both change
methods use `POST` because selectors and proof pagination are structured
payloads.

`from_block` is exclusive and `to_block` is inclusive. `to_block` resolves to
the finalized response anchor. Table selectors and account names must be
sorted, unique and non-empty. The request `cursor` and response `next` are
opaque `bytes`; callers store and return them unchanged. The projection owner
must bind a cursor to the chain, method, exact target anchor, normalized
selector/account set and internal proof position, and reject reuse in another
context.

`table_change_selector` carries `code`, `scope` and `table`.
`table_mutation` carries that selector, a primary ID and an optional
`table_row`; an absent row is a deletion. Each `table_change_batch` binds its
mutations to one `state_anchor`, and `table_changes_response.blocks` preserves
the ordered multi-block `(from_block, to_block]` history. Page limits apply to
the aggregate mutation count across all returned blocks.

`account_response` carries the canonical `full_account`, while
`account_mutation` carries an account name and optional canonical
`account_authority`; an absent authority is a deletion. `account_change_batch` and
`account_changes_response.blocks` use the same per-block history shape. Within
each block, mutations are last-write-wins by logical identity: a batch cannot
contain two mutations for the same table primary key or account.

The local projection verifier associates each batch anchor with its
authenticated change proofs and proves typed values, deletion, ordering,
ancestry, completeness and cursor continuation. Intermediate block anchors
must form a canonical sequence ending at the finalized target anchor on the
last page.
Unavailable retained history is reported as the typed
`history_unavailable` error so a consumer can bootstrap from current finalized
state or select an archival peer. The public Chain API does not expose the
runtime-specific `history_lost` concept.

## Local ABI conversion

`forge.chain.api.abi` owns local `forge::variant` JSON-value conversion for
`forge::chain::protocol::abi_def`. `abi_json_to_bin` and `abi_bin_to_json`
perform no network calls and require an explicit ABI and root type on every
invocation. The decoder rejects trailing bytes.

The traversal and wire rules are adapted from Spring commit `e6a99f68`,
`libraries/chain/abi_serializer.cpp` and
`libraries/chain/include/eosio/chain/abi_serializer.hpp`. Supported ABI shapes
include built-in Spring types, typedefs, struct inheritance, dynamic and fixed
arrays, optional fields, trailing binary extensions and tagged variants.
Public Forge names and error contracts are native; there are no EOSIO namespace
aliases or compatibility modes.

`abi_serialization_limits` bounds recursion depth, elapsed serialization time,
binary bytes, string/byte payloads and container elements. Failures throw
`abi_serialization_error` with a structured `abi_diagnostic` containing a
stable code, message, resolved type, logical JSON path and binary offset.
