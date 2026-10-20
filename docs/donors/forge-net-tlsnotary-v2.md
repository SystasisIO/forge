# Native TLSNotary session boundary

## Sources inspected

- `tlsnotary/tlsn@5f67389de3f8962227fddffd71d78a852acc9b01`:
  `crates/examples/basic/basic.rs`, `crates/tlsn/src/session.rs`, prover,
  verifier, TLS configuration and transcript APIs. This is the exact
  `0.1.0-alpha.16-pre` research revision qualified separately by the first
  downstream consumer; it is not a declaration of production cryptographic
  assurance.
- `tlsnotary/mpz2@8f897838f4e24aff9da8e8b012e8ba35ae770141`:
  process-global thread-pool initialization and MPC execution.
- Existing Forge `libraries/net/transport`: owned streams, cancellation,
  explicit close barriers and bounded reads/writes.
- Existing Forge `libraries/vm/wasm/optimizing`: `compiler_session_impl_child`,
  `compiler_session_impl_process` and `compiler_session_impl_worker` for
  `posix_spawn`, private local IPC, asynchronous process reaping and owned
  shutdown.
- Existing Forge App and Asio: component descriptors, dependency resolution,
  inherited plugin lifecycle, bounded compute work and asynchronous waiters.

The source references are immutable upstream revisions. Their cryptography is
used directly through Cargo's locked dependency graph; no local donor checkout
is a build dependency. The worker's [dependency inventory](../../libraries/net/tlsnotary/native/THIRD_PARTY.md)
records provenance and licenses.

## Accepted patterns

The native library owns a C++ asynchronous session boundary. The caller supplies
the already admitted peer stream and, for the prover, the server byte stream.
TLSNotary performs MPC-TLS over those bytes. An ordinary HTTP client's completed
response cannot be converted retrospectively into an authenticated transcript.
Products retain ownership of HTTP formatting, permitted endpoints, credentials,
task correlation and interpretation of authenticated response data.

One managed worker process owns one upstream session. The upstream session uses
a process-global MPZ pool without an owned teardown API. An embedded FFI would
leave that lifetime outside the library's shutdown barrier. Process isolation
allows a cancelled or stuck worker to be terminated and reaped without changing
upstream cryptography. The C++ host reuses Forge runtime/transport components
and the existing compiler-worker process ownership pattern; it does not create
a second network stack, general scheduler or generic process framework.

The private bridge uses bounded binary frames, per-channel read credit and write
acknowledgements. Raw MPC traffic does not pass through JSON, base64 or a guest
VM. Control, peer and server channels are separate logical directions over one
private connection; this is an implementation boundary, not another public
overlay protocol. The worker does not create network connections or read a
secret store.

The verifier independently configures roots, expected server name, disclosure
ranges and protocol resource limits. Prover completion and verifier acceptance
are different outcomes. Only the verifier result contains authenticated sparse
transcript bytes. A caller must still verify that those bytes satisfy its own
operation and accounting rules.

## Rejected patterns and limits

- No proxy-mode fallback, ideal cryptographic build, TEE requirement or API-key
  disclosure to the verifier.
- No TLS server changes and no replacement of existing Forge HTTP/P2P APIs.
- No task, attempt, provider pricing, settlement or application policy in Forge.
- No detached host work or successful shutdown while owned children remain.
- No upstream error text, payloads or credentials in diagnostics.
- No portable notarized attestation or on-chain proof verifier in this stage.
- Transcript, record, wire, thread, queue and deadline bounds are not an OS
  memory sandbox. Upstream intermediate allocations and its multiplexer window
  remain relevant; a measured RSS value is not a hard memory quota.

The library and official session plugin are opt-in and explicitly Experimental.
Their private worker protocol is not a stable external wire contract. Pinning
the upstream revision does not replace an audit of that revision or establish
the monetary cost of a complete verified operation.

## Acceptance boundary

Acceptance requires real MPC against a local TLS fixture through the C++ library,
independent verifier trust roots and identity checks, bounded admission,
cancellation/deadline/stop failure paths, redacted typed errors and an installed
consumer with the installed worker. A successful worker-only test is not a
substitute for plugin lifecycle or package installation checks. Platform and
upstream-service acceptance must be reported separately.
