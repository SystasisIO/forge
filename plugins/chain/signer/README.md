# Chain Signer Plugin

`forge::plugins::chain::signer` composes injected local signing providers into
the Chain transaction, block and finality signer contracts. It owns no private
keys, transport listener, HTTP/P2P publication, consensus state, or durable
block-signing safety record.

Composition can optionally supply `plugin_options.resolve_provider(name, apis)`.
At initialization it resolves each missing transaction/block profile provider
name exactly once into the existing named-provider list, before policy compilation.
Explicit providers are never replaced. Unknown/null providers and callback errors
fail configuration; diagnostics do not forward callback exception text. This is
a local initialization hook, not request-time fallback or a remote registry.
The application must declare the source plugin dependency. The analogous optional
`resolve_finality_provider(name, apis)` is awaitable and resolves the single missing
configured BLS provider during initialization. Explicit BLS providers still win;
empty names, null results and failures are rejected without leaking source diagnostics.
Neither hook changes the wire API or creates request-time fallback.

## Identity

- Target: `forge_plugins_chain_signer`
- Package component: `plugins_chain_signer`
- Plugin id: `forge.plugins.chain.signer`
- Transaction API id: `forge.chain.api.transaction_signer`
- Block API id: `forge.chain.api.block_signer`
- Finality API id: `forge.chain.api.finality_signer`
- Public modules:
  - `forge.plugins.chain.signer.plugin`
  - `forge.plugins.chain.signer.descriptor`
  - `forge.plugins.chain.signer.types`
  - `forge.plugins.chain.signer.exceptions`
  - `forge.plugins.chain.signer.block_execution_handler`

## Composition

The application injects named `forge::crypto::signer::provider` instances for
transactions and blocks, and named `forge::crypto::bls::signer::provider` instances for
finality. `plugin_options` carries public keys, provider names, exact
transaction policies, the selected finality provider, a semantic authorization
provider, and an admission limit. It never carries private key material or a
transport credential.

Every transaction action authorization must match one policy exactly on chain,
action contract/name, authorization actor/permission, and the server-supplied
caller source/fingerprint. Context-free actions, empty authorization lists,
missing rules, ambiguous provider selection, and cross-provider transactions
are denied. There are no wildcard rules and an empty policy set signs nothing.

The envelope allowlist intentionally does not interpret application ABI action
data. Remote transaction profiles default to `remote-authorization: semantic`
and require the injected `semantic_authorization_provider`; product composition
supplies this ABI-aware authorization before provider cryptography occurs.
`remote-authorization: profile_only` is an explicit opt-in that omits the
callback but keeps exact chain, caller, action and envelope checks. Local-only
profiles may omit the callback. `max-delay-seconds` defaults to `0`, denying deferred
transactions unless a profile explicitly opts into a bounded delay.

```cpp
import forge.plugins.chain.signer.plugin;

auto descriptor = forge::plugins::chain::signer::descriptor({
   .providers = {{
      .name = "transaction",
      .value = transaction_provider,
   }},
   .finality_providers = {{
      .name = "finality",
      .value = finality_provider,
   }},
   .semantic_authorization = semantic_authorization,
   .initial_config = {
      .transaction_profiles = {exact_policy},
      .block_profiles = {exact_block_policy},
      .finality = finality_binding,
      .max_inflight = 32,
      .max_queued = 128,
      .max_queued_bytes = 64U * 1024U * 1024U,
      .max_per_caller = 32,
      .shutdown_timeout_ms = 5000,
   },
   .audit = audit,
   .now = clock,
});
```

Both transaction and block contracts are remote-capable, but their caller is a required
server-supplied value. Bindings must authenticate a transport principal and put
`forge::api::auth::authenticated_caller` into `trusted_invocation`; clients do
not control that value. A block request supplies `chain`, canonical `header`
and 1–64 unique K1 public `keys`; signatures are returned in requested-key
order. A block profile matches exact chain, header producer and local allowance
or authenticated caller source/fingerprint. Every requested key must be bound
to a configured provider and key ID. The producer/controller selects current
authority keys and assembles any threshold or additional signatures; Forge
does not decide active authority or block validity. `max-header-bytes` defaults
to 1 MiB, is configurable up to 64 MiB, and is checked on canonical header
packing before provider calls. Block public keys cannot be configured across different chains: chain ID is
a policy selector, not a block-signature domain separator. The finality contract is local-only: `identity()`
returns typed public key and proof of possession, while `sign_vote(block_ref,
vote_kind)` returns a typed `finalizer_vote`, never raw BLS bytes or safety
state.

## Runtime

Cheap exact-profile selection runs before admission. Admission is bounded by
`max_inflight`, request count, queued decoded bytes and per-caller quota; the
queue byte budget is independent from each profile's final
`max_packed_bytes`. Caller cancellation and plugin stop propagate to active
provider operations. `request_stop()` denies new work and cancels queued and
active calls; `shutdown()` drains them within `shutdown_timeout_ms` before
providers can be released.

## Durable block-signing guard (product-owned)

The product must own one active signing process for each producer key, with
serialized signing attempts per `(chain, producer)` and independent progress
for different pairs. It must maintain exactly one bounded, durable last-slot
record for each pair, containing the canonical header, execution state
(pending intent or completed), and result. This is not a growing journal.
Inject the optional local `plugin_options.block_execution` handler to persist
intent **inside** the existing signer API after policy, admission and all key
identity checks. Persist the completed result before returning from the handler.
Forge then verifies signature count, order, algorithm and recovery again, even
for a cached response. Denied requests never enter the handler. No second signer
registration or wrapper in front of policy checks is needed.

```cpp
import forge.plugins.chain.signer.plugin;
import forge.plugins.chain.signer.block_execution_handler;

// durable_guard implements block_execution_handler::execute(request, signing).
// signing is a lazy move-only awaitable: await it once after durable intent,
// or discard it and return the exact previously persisted signatures.
auto descriptor = forge::plugins::chain::signer::descriptor({
   .providers = providers,
   .block_execution = durable_guard,
   .initial_config = {.block_profiles = profiles},
});
```

The handler participates in the existing request cancellation and drain scope.
It must await all work it starts and must not detach cryptography or durable
writes. The supplied operation expires on handler completion or failure;
retaining it cannot retain admission or authorize later signing. Cancellation
does not undo signatures already produced or durable writes already committed.
The product must resolve those uncertain outcomes conservatively on retry.
This extension is local composition only; it changes no remote signer contract.

After restart, a pending intent must be resolved conservatively:
only an exact retry of that same canonical header may continue, while a
conflicting block at the same slot, an older slot, or a storage error fails
closed. Local and remote entry points must pass through the same guard. Two
independent instances must never control the same producer key without shared
serialization and durable state. Forge does not provide a database, journal,
consensus validator, block semantic callback or guaranteed remote retry behavior.
The API returns all requested signatures or an error, but this is not atomic
cryptography: an earlier key may already have signed when a later key fails or
the caller loses the response. The product must treat ambiguous attempts as
consumed in its durable guard and must not blindly retry a changed block.

Donor traceability (inspected local snapshots):

- Spring `e6a99f68b`, `plugins/producer_plugin/producer_plugin.cpp:2974–3005`
  traverses the active producer authority and configured providers; `libraries/chain/block_state.cpp:71–81`
  signs the block ID and assembles main plus additional signatures. We accept
  multi-key selection and ordered signatures, but leave current authority and
  threshold assembly with the product.
- Forge base `8a277203`, `libraries/chain/protocol/block.cpp:62` provides
  `calculate_block_id`; `libraries/crypto/signer/include/forge/crypto/signer/provider.cppm`
  provides identity lookup and K1 signing; `libraries/chain/savanna/admission.cpp:23–59`
  verifies weighted producer signatures. The plugin reuses those contracts,
  `libraries/api/core` trusted invocation, HTTP mapping and P2P binding instead
  of introducing another signer transport or provider.
- CometBFT `2a9c48a`, `privval/file.go:74–143,303–420` stores and checks a
  last-sign state; `privval/signer_requestHandler.go:70–83` checks chain binding.
  We accept the fail-closed, durable last-sign principle only as a downstream
  product requirement. We do not transplant CometBFT's consensus-specific
  height/round/step format, file store, or retry client into Forge.
- Spring's generic digest endpoint and synchronous signature-provider HTTP
  transport are rejected: this API accepts only canonical typed transactions
  and block headers, with transport authentication supplied by existing Forge
  bindings.

## Source migration before stabilization

This explicitly approved pre-stabilization MINOR exception changes three C++
names without aliases: `named_transaction_provider` → `named_provider`,
`plugin_options.transaction_providers` → `plugin_options.providers`, and
`transaction_key_binding` → `key_binding`. The YAML binding fields
`provider`, `key-id`, and `expected-public-key` remain unchanged. No release is
performed by this change. The affected surfaces and mechanical downstream
rebuild path are recorded in the [UNRELEASED Chain signer release note](../../../docs/releases/unreleased-chain-signer.md).
