# Native TLSNotary sessions

## New optional components

`FORGE_ENABLE_TLSNOTARY=ON` adds Experimental native C++ sessions and the
official App session plugin. It requires Rust 1.95 to build the locked private
worker. Existing builds keep this feature disabled unless requested.

- Package `net_tlsnotary`, target `Forge::forge_net_tlsnotary`, modules
  `forge.net.tlsnotary.{types,exceptions,engine}`.
- Package `plugins_net_tlsnotary_session`, target
  `Forge::forge_plugins_net_tlsnotary_session`, module prefix
  `forge.plugins.net.tlsnotary.session`.
- Installed private executable `libexec/forge/forge-tlsnotary-worker`; the
  application configures its trusted absolute path explicitly.

The library composes pinned upstream MPC-TLS, takes caller-supplied Forge byte
streams and returns selected authenticated transcript ranges to the verifier.
It does not implement application authorization, HTTP interpretation, peer
discovery, pricing, notarization service or contract verification. See the
[library](../../libraries/net/tlsnotary/README.md),
[plugin](../../plugins/net/tlsnotary/session/README.md) and
[donor/ownership note](../donors/forge-net-tlsnotary-v2.md).

## TCP stream correction and migration

The generic stream returned by `forge::net::tcp::connection` now implements
`async_close_write()`: it sends FIN while preserving the read direction. Writes
after FIN fail; repeated write closure is harmless. Full close and cancellation
retain their terminal behavior.

An orderly EOF at this **generic transport stream** boundary now reports the
existing `forge::net::transport::exceptions::end_of_stream`. It previously
reported `forge::net::tcp::exceptions::closed`, which also covered reset and did
not let an MPC transport distinguish them. Callers that caught TCP `closed` as
EOF for a generic stream must catch transport `end_of_stream` for graceful EOF;
a reset remains a failure. The typed TCP `connection` API is unchanged. No error
alias or string-based classification is added.

## Shared child signal ownership

The native session engine and the optional WASM optimizing compiler now reuse
`forge.asio.child_signal_registration` on macOS. It owns only validated,
shared Asio `SIGCHLD` registration; child launch, reaping and cleanup remain
with their existing owners. Concurrent runtimes share that registration.
Foreign handlers are rejected without replacement.

The cold process disposition must be exactly `SIG_DFL` with zero flags and an
empty mask. The previous private compiler guard also accepted some custom
default-handler flags/masks which Asio would overwrite. Such configurations
now fail explicitly; clear custom `SIGCHLD` configuration before acquiring the
guard. Existing VM boundary exception categories and process/session APIs are
unchanged. Release is explicit after all owned process handles have drained.

## Qualification boundary

This is an Experimental integration of an exact research upstream revision,
not a cryptographic production certification. Resource limits cover admission,
protocol bytes/records, worker threads and deadlines; they are not an OS memory
sandbox. Portable proofs, monetary cost qualification and platform/upstream
service acceptance are separate gates.
