# UNRELEASED: wallet custody and block execution

No version/tag/release is performed by this work.

New surfaces: `forge.crypto.wallet` management API 1.0, its Crypto Wallet runtime
plugin, and optional local Chain Signer `block_execution_handler`. Existing
transaction/block signer wire contracts and encrypted keystore bytes are unchanged.

Keystore ownership is now exclusive even for one-shot clients. Simultaneous
independent opens fail with `in_use`; a long-running wallet owner retains its
ownership capability across decrypted-store destruction and explicit unlock.

Runtime migration: path-based `store::open` now requires an owner-only parent
directory (previously only the opened file had to be private). Stop all clients,
move an existing encrypted file from a shared/non-private directory into a
private `0700` directory, then change the configured path. Do not restore or
replace the sibling lock file while any owner is active. No automatic migration,
format rewrite or aliases are supplied.

The added `*_sync` store methods share the existing provider implementation,
allowing wallet composition to offload all operations to Forge's bounded compute
pool without creating a runtime in the crypto library.

Configuration-driven applications may resolve existing wallet providers through
the local-only `forge.plugins.crypto.wallet.provider_source` API. Chain Signer's
optional initialization callback resolves only names referenced by its profiles;
the application declares the Wallet dependency explicitly. No signer wire change,
remote lookup, implicit unlock or request-time fallback is introduced.

## Savanna checkpoint recovery compatibility

The combined candidate also repairs retained validation history for delayed QC
claims below the finalized anchor. Checkpoint field layout and validation Raw
version remain unchanged, but checkpoints can retain a wider complete history
than `[N,N]`. Old readers requiring that single-root range are semantically
incompatible; upgrade readers and checkpoint producers together.

When used as a finality bootstrap, incomplete legacy history fails closed as
`untrusted_finality_bootstrap` (Chain API `trust_required`). Direct
`validate(checkpoint)` / `equivalent()` validation instead throws
`invalid_validation_state`. Recover from an earlier independently trusted seed
and verified canonical history. There is no automatic reset or migration and no
change to consensus/block/signature formats. See the
[Savanna compatibility details](../../libraries/chain/savanna/README.md).
