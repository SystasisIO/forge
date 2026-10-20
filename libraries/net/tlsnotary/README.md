# TLSNotary native sessions

Experimental C++ source API over upstream TLSNotary at the exact commit recorded
in [native/Cargo.toml](native/Cargo.toml) and [native/Cargo.lock](native/Cargo.lock).
Enable `FORGE_ENABLE_TLSNOTARY=ON`; link `Forge::forge_net_tlsnotary` and request
the installed `net_tlsnotary` package component. This is host code only.
Consumers which also import Asio modules link `Forge::forge_asio` explicitly.

Use this library when two peers must jointly authenticate selected TLS transcript
bytes without disclosing the remaining request bytes to the verifier. Applications
own the meaning of those bytes, source approval, credentials, replay protection
and decisions about external effects. An authenticated transcript is not an
application authorization decision.

## API and ownership

The public modules are `forge.net.tlsnotary.types`, `.exceptions` and `.engine`.
The caller owns Forge Asio I/O runtime and compute workers and keeps them alive
until `engine::shutdown()` completes. The engine owns transferred peer and raw
server streams, bounded admission and one private managed process per active
session. The process performs actual MPC-TLS, not verification of a previously
fetched ordinary HTTPS response. It owns no network sockets, server framework,
P2P discovery, application jobs or credentials store.

```cpp
import forge.asio.runtime;
import forge.asio.compute;
import forge.net.tlsnotary.engine;

namespace tlsn = forge::net::tlsnotary;

// runtime and compute are caller-owned; worker is a trusted absolute path.
tlsn::engine sessions{runtime, compute.get_executor(),
   {.worker_path = worker, .max_active = 2, .max_queued = 8}};

tlsn::verifier_request request;
request.peer = std::move(authenticated_peer_stream);
request.source = {.server_name = "api.example.org",
                  .trust_roots_der = trusted_roots_der};
request.options.id = 42;
request.options.reveal_sent = {{0, 32}};
request.options.reveal_received = {{0, 0xffffffff}};
auto facts = co_await sessions.verify(std::move(request));
// Interpret facts.sent/facts.received under the application's own policy.
co_await sessions.shutdown();
```

`prove(prover_request)` additionally takes a raw connected server stream and
bounded request bytes. Those bytes are sent inside TLS established by TLSNotary.
There is no implicit HTTP parser, redirect, retry or destination selection.
The server must finish its TLS response; a persistent protocol which never closes
will reach the explicit deadline. TLS 1.2 and the upstream supported cipher/profile
restrictions apply. `deferred_decryption` selects the upstream mode explicitly.

Both roles require their own expected server identity and DER trust roots. Roots
received from a remote prover must never become trusted configuration. Disclosure
ranges are ordered, disjoint half-open byte ranges; end `0xffffffff` means the
actual end of that transcript direction. The verifier requires the exact agreed
mask. Empty masks are supported and return authenticated server identity and
transcript lengths without revealing bytes. This identity-only profile is not
semantic evidence for an application request or response: the application must
require the disclosure ranges needed by its policy. A verifier result contains only authenticated bytes and their offsets, server
identity and total direction lengths. Hidden bytes are absent. Prover completion
returns source/length metadata and empty authenticated-range collections; only a
verifier result is evidence for the peer. This API defines no portable persisted
proof format, signed notarization service or offline attestation verifier.

## Limits and shutdown

Each session has a nonzero local ID, one absolute deadline covering lazy entry,
queue time, spawn and protocol work, byte/record/thread bounds, and a stop token.
`options.stop`, `cancel(id)` and `request_stop()` are the authoritative
cancellation inputs. Ambient Asio coroutine cancellation is isolated while the
accepted resource owner drains; emitting it does not detach a native session or
replace explicit session cancellation. `cancel(id)` cancels a queued or active session; `request_stop()` closes admission
and cancels all work; `shutdown()` joins accepted entries, stream cleanup, timers
and child reaping. Destruction requests stop but is not the asynchronous shutdown
barrier. Native sockets, timers and processes are created only after a polled operation
enters admission. A retained lazy operation owns its engine state, not a raw
engine pointer; after engine shutdown it rejects polling before touching native
runtime resources. An unpolled awaitable still holds the caller-supplied streams:
destroy or finish all such awaitables before destroying those streams' runtimes.
Unpolled operations have not been accepted and are not engine shutdown work.

Successful native completion drains the existing bridge writes and write-FIN
acknowledgements before releasing its result. The host then joins the child and
closes the raw streams gracefully under the same session deadline. Terminal
stream cancellation and worker termination remain the failure, timeout and
explicit cancellation paths; successful cleanup must not reset a QUIC stream
whose final writes are still queued.

The C++ side bounds active/queued sessions, retained root/range/request capacities, frame
sizes and raw stream bytes/capacity before copying. A logically small vector with
an oversized reserved allocation is intentionally rejected and released. Credit-based binary IPC bounds each
channel to one read credit and one unacknowledged write. Transcripts are capped
at 65,536 bytes per direction; roots at 64 and 1 MiB combined; each root at
65,536 bytes; ranges at 1,024 per direction. The upstream
`max_received_records_online` bound applies to online decryption only; it is not a
claim about all deferred TLS records. Native validation also bounds encrypted
server traffic. No numeric memory-containment promise is made: upstream allocates
additional cryptographic/multiplexer memory; see [the donor note](../../../docs/donors/forge-net-tlsnotary-v2.md).

Compute admission is reused for short process launch and setup rollback. It is
released after the strand installs the asynchronous Boost.Process reaper, not
held throughout MPC. The embedding process must not reap these children or enable
`SIGCHLD=SIG_IGN`/`SA_NOCLDWAIT`. Reap, kill and PID authority clearing use the same
strand. No blocking destructor, detached thread, polling loop or fresh cleanup
compute admission is used.

The installed worker is `libexec/forge/forge-tlsnotary-worker` under the selected
install prefix. Supply that trusted absolute path explicitly; no PATH lookup,
peer-provided executable or source-tree fallback exists. The current process
adapter is qualified on macOS only. Linux qualification remains separate.

## Security and validation

Obtain secrets through the existing host secret capability, build a bounded
request in host memory and transfer it to `prove`; no key should enter diagnostics,
configuration dumps or a guest. Request bytes are sensitive ordinary owned memory,
not a secure-erasure guarantee. The library logs no payload, key, source path or
native stderr. External stream failures are converted at the call boundary to
static typed `forge.net.tlsnotary` errors, including cleanup errors.

`test_forge_net_tlsnotary` runs actual paired native sessions against an in-process
TLS fixture, selective disclosure, independently wrong roots/name/mask, worker IPC
faults, bounds, cancellation, overload and cleanup. Its optional first argument
selects the worker, so the same checks can validate the installed executable.
`test_forge_plugins_net_tlsnotary_session` exercises the official App plugin.
The independent installed consumer requests only installed public package leaves.
Rust worker tests cover strict binary parsing and native input limits.

A local technical pass does not approve this upstream research version for
production, establish Linux compatibility or prove a commercial cost threshold.
