# Chain outbox plugin

Preview API and storage format. The plugin owns delivery and restart recovery for
the client-side Chain API. It uses the installed transaction signer and an
explicit named ObjectDB store. The remote node exposes the existing Chain
query and submission services over HTTP or P2P.

Set `plugins.chain.outbox.store` to a name configured under
`plugins.db.store.stores`. The named store must expose its Object layer.
`plugin::descriptor(plugin_options)` supplies the verified chain configuration
and can supply a per-method delivery policy and an admission callback.
Select a durable backend for the outbox. The plugin synchronizes the database
before sending a packet.

Applications using `plugin_options::deferred_configuration` can call
`setup::configure_read` during initialization. The profile binds a typed Chain
API to a destination, verified client options, a concurrent call limit and a
deadline. It selects its own chain, trust root and state domain. With only a read
profile configured, startup opens the read transport without opening the named
delivery store or resolving a signer. A typed read for this API rejects a
different destination. Typed APIs without a read profile use the default chain
verification options.

Configure named HTTP endpoints under `plugins.chain.outbox.http`.

```yaml
plugins:
  chain:
    outbox:
      store: orders
      http:
        - name: primary
          url: https://chain.example/spine
```

The URL may include a path prefix. Forge appends the existing `/v1/chain/...`
routes and uses the HTTP client's TLS verification for HTTPS. Applications that
use HTTP exclusively pass `plugin_options::p2p = false` to `descriptor(...)` and
can omit the P2P plugins. P2P stays enabled by default.

An HTTP outbox record refers to the configured endpoint by name. Keep that name
configured while its transactions are pending. If it is absent, a staged packet
keeps its first-send state until the endpoint is restored or the packet expires.
The queue verifies the configured chain before sending a packet. The delivery
policy in the record controls POST replay after an uncertain result. The HTTP
client does not retry the submission POST itself.

The admission callback runs inside the same ObjectDB transaction as the outbox
insert. Use it for local checks and writes that must commit with delivery admission.
Keep remote calls outside this database transaction. If the callback throws,
the transaction rolls back. A submission acknowledgement yields a transaction ID.
Use the Chain binding's status operation to fetch the verified final receipt.
A timeout can leave the transaction queued for recovery.

Set `plugin_options::delivery.prepare_first_send` when an application must
recheck a staged packet immediately before its first possible send. Preparation
can obtain evidence outside the writer. The returned check can atomically
record local application state with the delivery decision. Register the same
handler at startup so the check applies to packets restored after restart.
See [the outbox decisions](../../../libraries/api/chain/README.md) for details.

The `forge_plugins_chain_outbox` target is provided by the
`plugins_chain_outbox` package component. Import
`forge.plugins.chain.outbox.plugin` to configure and register it. See the
[Chain API library](../../../libraries/api/chain/README.md) for contract bindings.
