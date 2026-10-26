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

## Retained peer writes and diagnostics

Native MPC can produce peer bytes faster than a lower transport releases queued
writes. A successful `async_write` does not necessarily release the chunk's
resource reservation. The native bridge now uses a private 1 MiB sliding byte
window, released by the existing chunk lifetime, to pace that producer. It moves
owned frames into transport chunks and admits further bytes when a retained
chunk is released; it does not wait for a peer acknowledgement after every frame.

The window uses the existing session deadline and cancellation path. Cleanup
joins worker and bridge activity, while late chunk release retains only the
independent window state. There is no retry, quota increase, public API change
or wire-format change. Custom and concurrently shared P2P budgets may still
reject a write.

Safe diagnostics distinguish the failing TLSNotary channel and operation from
the underlying typed transport error. P2P memory-admission diagnostics report
the rejecting scope, dimension, limits and counters. They exclude payloads,
credentials, peer identifiers and arbitrary exception messages.

The native package leaf now declares its existing logger dependency explicitly,
including package discovery. Retaining-stream tests cover the full window,
single-chunk resumption, late release, cancellation, deadline and read/write
failure. Those ownership tests are separate from the real MPC fixture tests.

## Qualification boundary

This is an Experimental integration of an exact research upstream revision,
not a cryptographic production certification. Resource limits cover admission,
protocol bytes/records, worker threads and deadlines; they are not an OS memory
sandbox. Portable proofs, monetary cost qualification and platform/upstream
service acceptance are separate gates.

## Retained commitments and opening verification

Additive experimental source API: session options can explicitly request up to
16 SHA-256 transcript commitments. Results optionally return versioned retained
evidence; secret openings are returned to the prover only. The local session API
is 1.1 and adds `check(check_request)` using the existing managed worker and
lifecycle. The original default FTN1 protocol is unchanged; opt-in FTN2 requires
matching installed host/worker binaries. No previous persisted evidence format
existed. New records use canonical Raw, exact bounded decoding and format 1.

The upstream TLSNotary/MPZ pins do not change. No upstream fork, new crypto
implementation, notarization service or financial/contract mechanism is added.
Offline checks bind openings to caller-authenticated commitments; they do not
independently authenticate a TLS source. Applications sign or trust the complete
record with their own context before allowing disclosure. Local macOS evidence
must be assessed separately from Linux or commercial production acceptance.

## Values without the native worker

The additive `net_tlsnotary_values` component exports
`Forge::forge_net_tlsnotary_values` and the existing
`forge.net.tlsnotary.types` module even with `FORGE_ENABLE_TLSNOTARY=OFF`.
It supplies passive values for retaining session outcomes; it neither starts a
worker nor provides a session engine. Existing consumers of `net_tlsnotary`
retain the same imports through its public dependency on this component.

`authenticated_range` and `result` now declare their fields for canonical Raw
serialization. A retained verifier result contains authenticated public ranges
and commitments. Prover results can also contain secret openings: applications
must protect those values and must not publish the whole serialized object.
Deserialization does not reauthenticate a transcript or create a portable proof.
The application still owns the trusted origin of a retained result, its binding
to a request, and disclosure policy. FTN1/FTN2 and upstream pins are unchanged.

The official `package_net_tlsnotary_values_component` consumer checks the public
component and exact Raw field order, separately for verifier and prover values.
Build-tree, installed-package and runtime qualification are separate gates.
