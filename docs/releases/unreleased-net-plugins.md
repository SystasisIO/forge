# Network plugin families: migration to net

This maintainer-approved clean rename is a scoped pre-stabilization MINOR
exception. It is not a PATCH-compatible refactor. No version or release tag is
created by this change, and no compatibility aliases are provided.

## Mechanical mapping

Insert `net` between `plugins` and `http` / `p2p` for these five leaves:

| Old leaf | New leaf |
|---|---|
| `http/server` | `net/http/server` |
| `p2p/node` | `net/p2p/node` |
| `p2p/resolver` | `net/p2p/resolver` |
| `p2p/diagnostics` | `net/p2p/diagnostics` |
| `p2p/pubsub` | `net/p2p/pubsub` |

For example, `forge::plugins::http::server` becomes
`forge::plugins::net::http::server`, module prefix
`forge.plugins.http.server` becomes `forge.plugins.net.http.server`,
target `forge_plugins_http_server` becomes `forge_plugins_net_http_server`,
and package component `plugins_http_server` becomes
`plugins_net_http_server`. Public include paths follow the same mapping.
Update all dependencies, imports, plugin registrations and explicit API lookups.

## Configuration and startup

Move existing settings intact, including enabled flags, TLS secret references,
listeners, peer identity, authorization and resource limits:

```yaml
plugins:
  net:
    http:
      server:
        enabled: false
    p2p:
      node:
        enabled: false
```

CLI flags now use e.g. `--plugins.net.http.server.port`. Regenerate application
help/configuration and update environment overrides using the application's
existing field mapping. Old sections are not accepted as aliases. Each enabled
network plugin rejects its former config section before decoding new settings,
including mixed old/new configuration, to prevent silently applying defaults
instead of an operator's former TLS or network policy.

## API and wire compatibility

Plugin IDs and plugin-owned API IDs change with the namespace, including
`forge.plugins.net.p2p.resolver.protocol` and other existing suffixes.
Their methods, payload layouts and numeric API versions are unchanged.
Applications explicitly requesting the old IDs must migrate; restart participants
together when using the resolver control API. Do not claim mixed-version resolver
compatibility merely because its protocol route remains `/forge/api/resolver/2`.

Library contracts in `forge.api.*`, `forge.chain.api.*`, and
`forge.crypto.wallet.api` are unchanged. Transport implementations in
`forge.net.*`, libp2p protocol IDs, HTTP routes, request framing, authentication,
TLS policy and operation authorization are unchanged.

## State and packaging

No persisted schema, object IDs, field ordering, state directory, wallet, signing
record, peer identity or key material is migrated or reset. Keep existing data.
Use a fresh installation prefix for package verification: an incremental CMake
install does not remove obsolete files from an older installation.

`net`, `net.http` and `net.p2p` are empty groupings, not new runtime components.
The family rename initially retained five leaf plugins. Stage 7 subsequently
consolidates the P2P leaves as described below; there is no aggregate network
plugin or parallel networking implementation.

## Stage 7 P2P Consolidation

This additional maintainer-approved source/package/configuration break removes
the standalone PubSub and diagnostics plugins. The node plugin owns their
consumer behavior; the resolver remains an independent optional plugin.

| Previous surface | Replacement |
| --- | --- |
| `plugins_net_p2p_pubsub`, `plugins_net_p2p_diagnostics` components | `plugins_net_p2p_node` |
| `forge_plugins_net_p2p_pubsub`, `forge_plugins_net_p2p_diagnostics` targets | `forge_plugins_net_p2p_node` |
| PubSub/diagnostics plugin descriptors | Remove from application registration; retain the node descriptor |
| `forge.plugins.net.p2p.pubsub.api` / `pubsub::api` | `forge.plugins.net.p2p.node.pubsub_api` / `node::pubsub_api` |
| `forge.plugins.net.p2p.diagnostics.api` / `diagnostics::api` | `forge.plugins.net.p2p.node.diagnostics_api` / `node::diagnostics_api` |
| `forge.plugins.net.p2p.diagnostics.events_api` | `forge.plugins.net.p2p.node.events_api` |
| `node::{pubsub_source,diagnostics_source,host_event_source}` | Use the corresponding consumer API directly; the intermediary contracts are removed |
| `plugins.net.p2p.pubsub` and `plugins.net.p2p.diagnostics` settings | Nested `plugins.net.p2p.node.pubsub` and `.diagnostics` settings |

The node plugin version becomes `8.0.0`; the new role contracts start at
`1.0` under `forge.plugins.net.p2p.node.{pubsub,diagnostics,events}`.
Existing unaffected node and resolver contracts keep their versions.
These local contracts do not change libp2p wire formats, protocol IDs, native
GossipSub algorithms or the persisted peer-state schema. Remove old installed
package files by deploying to a clean prefix rather than relying on incremental
install to delete retired modules. No compatibility aliases are supplied.
