# forge.crypto.wallet

Typed wallet-management contract, independent of Chain transactions and product
storage. Target/component: `forge_crypto_wallet` / `crypto_wallet`.

Modules: `forge.crypto.wallet.protocol` (network records and stable enum values),
`forge.crypto.wallet.api` (wallet API 1.0 and HTTP mappings), and
`forge.crypto.wallet.exceptions` (typed public failures).

The runtime belongs to the Crypto Wallet plugin; this library owns no keys,
filesystem, timers, listeners or worker threads. Management requires an exact
mTLS caller and wallet/operation permission. It does not grant signing permission.
Do not publish this API through P2P or a non-mutual HTTP listener.

All methods use POST /v1/wallet/<method> and no-store responses. The authenticated
caller is server-supplied and is never an input field. Passwords and private keys
appear only in protected request bodies, never URLs, argv, ordinary environment
variables, audit events or exception context. Schema secret metadata is not
automatic redaction of an arbitrary network payload: never log these bodies or
serialize them for diagnostics. Transport buffers are not promised to be erased.

Example (a configured mTLS proxy or local test handle named `wallets`):

```cpp
import forge.crypto.wallet.api;

auto status = co_await wallets->status({.wallet = "producer"}, {});
// Remote bindings inject the authenticated caller; {} is not remote authority.
// create/open return locked. list_public_keys fails with typed locked until unlock.
```

No private-key export, digest-signing method or profile-editing operation exists.
The keys returned by create/import are public records only; their ids are wallet
management identifiers, not client-controlled remote transaction-signing key ids.
