# Crypto Wallet plugin

`forge.plugins.crypto.wallet.plugin::plugin` implements `forge.crypto.wallet.api`
using the existing encrypted Forge keystore and application task scheduler.
Its descriptor is opt-in. HTTP publication additionally requires an existing
HTTP server configured with mutual TLS; the plugin does not own a listener.
No P2P management binding is published.

An embedding host can set `plugin_options.allow_http_management=false` to prohibit
management publication altogether. `publish-http: true` then fails configuration;
YAML cannot override this composition policy. Local provider use remains available.

An application can inject stable `provider` instances in `plugin_options.wallets`
and pass the same instances to the existing Chain Signer named-provider list.
These providers survive lock/unlock. API-created wallets without a configured
Chain Signer binding remain management-only: creating a wallet does not grant
signing authority or edit a signer profile.

For configuration-driven composition, the plugin publishes the **local-only**
`forge.plugins.crypto.wallet.provider_source` capability in the existing API
registry. `get(name)` returns that same provider and rejects unknown names; it
never creates, opens or unlocks a wallet. The composing application makes Wallet
a dependency of Chain Signer and supplies `plugin_options.resolve_provider`,
which resolves configured provider names through this capability at initialization.
Chain Signer resolves only names referenced by transaction/block profiles and
retains the resulting providers. There is no request-time fallback or remote
provider discovery. This capability is not an HTTP/P2P wallet-management API.

```yaml
plugins:
  crypto:
    wallet:
      enabled: true
      directory: /srv/keys/wallets
      publish-http: true
      max-wallets: 32
      max-pending: 64
      max-request-bytes: 32768
      wallets:
        - name: producer
          open: true
          timeout-seconds: 0
          unlock-file: /run/secrets/producer-password
      permissions:
        - fingerprint: "<64 lowercase hex characters: client certificate SHA-256>"
          wallets: [producer]
          operations: [status, unlock, lock, list_public_keys]
```

Names contain only ASCII letters, digits, `_` and `-`, at most 64 bytes. Clients
cannot choose paths. Fingerprints and wallet names are exact, with no wildcard.
`list` filters its result by permission; `lock_all` locks only wallets explicitly
granted for that operation. Local or P2P callers cannot bypass the mTLS management
policy. Program composition uses the local provider lifecycle, not forged callers.

Create/open/restart leave stores locked. Locked public-key enumeration fails.
The default idle timeout is 300 seconds. Successful signing and key creation,
import or removal refresh it; failed requests and status/key-identity queries
do not. Explicit unlock and timeout changes start a new idle interval.
Timeout zero plus an explicit protected startup password file
supports unattended operation. Startup unlock runs once, never after manual lock.
Each wallet serializes operations through a Forge gate, with a bounded queue.
Idle deadlines use bounded delayed Forge scheduler tasks, not permanent running
tasks. Ordinary successful key use updates the deadline without accumulating
canceled tasks. Refusal to schedule expiry locks the wallet. Each request also
enforces its current deadline before using keys, even under scheduler saturation.
Keystore I/O and cryptography execute on the application's bounded Forge compute
pool, never on I/O workers or through nested task-scheduler admission.
Application composition must supply that pool; there is no hidden fallback.
Cancellation drains submitted blocking work before releasing the wallet gate.
Lock/open reserve queue admission before revoking signing; after revocation,
they finish draining and locking even if the requesting client disconnects.
The daemon owns the persistent keystore lock across lock/unlock and file replace.

The network API uses typed sanitized errors, never dependency exception text.
Password/import fields are marked secret in schema; raw network-body logging is
forbidden. This does not promise erasure of all TLS/HTTP temporary buffers.
Set bounded HTTP request-body limits independently of this plugin's decoded
request limit. Do not put secrets in configuration, environment, URLs or argv.

An uncertain durable write or lost file ownership faults the wallet. Operator
recovery and a restart are required; an API unlock does not clear that fault.
