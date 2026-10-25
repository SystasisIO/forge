# TLSNotary session plugin

Experimental App lifecycle owner for [native TLSNotary sessions](../../../../libraries/net/tlsnotary/README.md).
Enable `FORGE_ENABLE_TLSNOTARY=ON`; link
`Forge::forge_plugins_net_tlsnotary_session` and request the installed
`plugins_net_tlsnotary_session` component.

The leaf namespace, module prefix and contract identity are
`forge.plugins.net.tlsnotary.session`; target and component names follow that
same path. Public modules are `.plugin`, `.api`, `.config` and `.exceptions`.
There is no aggregate network plugin or TLSNotary listener/server.

```cpp
import forge.app.application_builder;
import forge.plugins.net.tlsnotary.session.plugin;
import forge.plugins.net.tlsnotary.session.api;

auto builder = forge::app::application_builder{};
builder.compute({.worker_threads = 2})
   .plugin(forge::plugins::net::tlsnotary::session::plugin::descriptor());
auto shell = std::move(builder).build();
// Configure plugins.net.tlsnotary.session.worker-path to the trusted installed
// libexec/forge/forge-tlsnotary-worker path before starting the shell.
co_await shell->startup();
auto sessions = shell->apis().get<forge::plugins::net::tlsnotary::session::api>(
   forge::plugins::net::tlsnotary::session::api::ref());
auto facts = co_await sessions->verify(std::move(request));
co_await shell->shutdown();
```

Configuration under `plugins.net.tlsnotary.session` contains `worker-path`,
`max-active` (1–64) and `max-queued` (0–1024), plus App-owned `enabled` selection.
The path must be absolute; there is no implicit current-directory or PATH search.
App provides the I/O runtime and compute executor. The plugin publishes its local
typed API during provide, creates the engine at startup, closes admission at stop
and joins native sessions before releasing resources at shutdown. Retained APIs
reject use after stop. No parallel lifecycle model or worker pool is introduced.

The local API accepts move-owned generic peer/server streams and the library's
source constraints and disclosure ranges. Streams and stop tokens are local
resources, so the API is deliberately not an automatically exported RPC endpoint.
Applications compose existing HTTP/P2P routes and resolve existing secret APIs;
this plugin neither admits arbitrary peers nor authorizes external effects.
Credentials remain in the prover host; callers must never log request bytes or
interpret prover completion as verified evidence.

`test_forge_plugins_net_tlsnotary_session` covers shell startup, real worker
admission, overload, explicit cancellation, shutdown and retained API refusal.
`test_forge_net_tlsnotary` covers cryptographic/source/disclosure and fault paths.
The independently installed consumer verifies module imports and leaf linkage.
macOS local checks are separate from Linux and production qualification.

## Retained commitments

Local API 1.1 adds `check(check_request)` alongside `prove` and `verify`.
Commitment selection and returned evidence/openings are the existing native
library values, without a second DTO or storage/runtime. The plugin delegates
the operation to the same owned engine. Read the [native library contract](../../../../libraries/net/tlsnotary/README.md#retained-commitments-and-later-openings)
before storing or transferring evidence: a retained record is not an offline TLS
proof, and imported metadata requires independent application authentication.
