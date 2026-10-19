# Forge Swarm API (Preview)

Target `forge_swarm_api`, package component `swarm_api`, module
`forge.swarm.api.peer`, namespace `forge::swarm::api`.

`peer` declares contract `forge.swarm.peer` 1.0. It uses existing Forge.API live
streams, not a second framing or transport stack:

```cpp
import forge.swarm.api.peer;
import forge.swarm.protocol.metadata_assembler;

// handle and id come from the application's connection and content selection.
auto call = co_await handle.async_open<&forge::swarm::api::peer::metadata>(
    forge::swarm::protocol::metadata_request{id, "public"},
    forge::api::auth::authenticated_caller{});
auto assembly = forge::swarm::protocol::metadata_assembler{id};
while (auto chunk = co_await call.async_read()) assembly.append(*chunk);
co_await call.async_finish(); // Mandatory: EOF alone does not prove success.
auto validated = std::move(assembly).finish();
```

- `metadata(request, caller, stream_writer<metadata_chunk>)` transfers the
  canonical Core descriptor incrementally. Assembly verifies the requested ID.
- `exchange(request, caller, duplex_stream<peer_message, peer_message>)` has
  independently closable input/output directions. Read and write concurrently;
  sequential write-all/read-all can deadlock under receiver-driven backpressure.
- `cancel()` cancels one call; call destruction cancels an unfinished call.
  Always await `async_finish()` to observe terminal success or typed failure.

Local outbound item rejection cancels only that call. A hostile wire item
rejected by the inbound descriptor decoder is a session-level protocol failure
in the existing Forge.API reader loop; it closes that session. Do not describe
this as call-local isolation. Ordinary handler errors and call cancellation
still leave neighboring calls usable.

The generated descriptor installs bounded Protocol decoders in both relevant
directions, including the generated remote proxy. Each API item uses the canonical
Raw representation, without a Swarm length prefix or compatibility codec. HTTP/1.1 does
not support the duplex method and must reject such a binding explicitly.

## Authentication Is Not Admission

`authenticated_caller` is the existing required server-supplied field. A remote
binding must create it from verified transport identity in `trusted_invocation`.
Client-supplied source/fingerprint values are cleared; absence of trusted
authority fails before the handler. The P2P binding already supplies a peer-ID
fingerprint after TLS/Noise verification. Core does not depend on P2P.

A fingerprint, content ID or realm is not permission to serve. A production
implementation must enforce product admission and shared quotas, including on
metadata and resumed transfers. Local typed calls are direct: implementations
must check `transport_authenticated()` themselves. No fallback permits unknown
callers. No payment credentials belong in these generic DTOs.

The abstract contract owns no storage, DB, worker loop, node, registry or policy.
PR6 supplies the production service and request/session state machine. Stateless
Protocol validation does not enforce first-handshake ordering, request ownership,
availability or received-block verification. Terminal failures reuse Forge.API's
typed error channel; there is no success/error envelope inside stream items.

## Checks

`test_forge_swarm_api` runs real TCP/Forge.API exchanges: bounded incremental
metadata, simultaneous duplex traffic, both half-close orders, forged/missing
caller context, malformed items, cancellation isolation and terminal errors.
The independent installed `swarm_api` consumer checks the reachable link/module
dependency closure for DB/storage/P2P, not mere presence in the common export set.
This is Forge protocol testing, not BitTorrent or Bitswap wire interoperability.
