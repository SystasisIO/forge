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
The five leaf plugins retain independent targets and lifecycle; there is no
aggregate network plugin or parallel networking implementation.
