# UNRELEASED — Chain signer source migration

This is a proposed note for the maintainer-approved, scoped pre-stabilization
`MINOR` source break. It does not assign a version, create a release, or change
published release notes.

## Affected public source surfaces

The `forge.plugins.chain.signer.types` module changes these C++ names without
compatibility aliases:

| Previous source | Replacement |
| --- | --- |
| `named_transaction_provider` | `named_provider` |
| `plugin_options.transaction_providers` | `plugin_options.providers` |
| `transaction_key_binding` | `key_binding` |

Update each C++ declaration, initializer, and member access mechanically, then
rebuild the Chain signer plugin and downstream package consumers against the
new Forge headers and module interfaces. Injected K1 providers can now serve
both transaction and block profiles. The YAML binding fields `provider`,
`key-id`, and `expected-public-key` do not change. Existing transaction signer
method signature, API descriptor version 1.0, and transaction wire layout do
not change. No persisted format or package component is renamed.

The new `forge.chain.api.block_signer` contract uses the canonical
`forge::chain::protocol::block_sign_request` from
`forge.chain.protocol.block_signing` and adds the typed HTTP route
`/v1/signer/sign_block`; it does not replace a previous block signer wire
contract. Rebuild consumers importing the new protocol and API modules. For
configuration and application-owned signing-safety requirements, see the
[Chain signer plugin guide](../../plugins/chain/signer/README.md).
