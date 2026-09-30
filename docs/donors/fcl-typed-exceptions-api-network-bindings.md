# FCL Typed Exceptions And API Network Bindings Donor Notes

This iteration strengthens FCL around typed exception identity and API contracts
that can work both in process and over transports.

## Donors Inspected

- FC / BitStore style `api<impl>`: one C++ contract shape that can be consumed
  locally or remotely.
- gRPC / Connect: typed service contracts, status projection and transport
  bindings that are separate from service identity.
- Smithy: contract description is transport-neutral; HTTP receives explicit
  native mapping instead of a forced generic RPC body.
- Cap'n Proto: message-oriented protocols benefit from a canonical frame and
  stable call identity.
- libp2p: protocol negotiation is a named artifact owned by the P2P runtime;
  higher-level API sessions attach to the accepted stream instead of replacing
  node identity, relay, discovery or peer-store behavior.
- QUIC: streams and connection limits are transport policy, while API deadline,
  cancel, call identity and stream lifecycle are protocol-above-transport policy.
- OSGi Declarative Services: application/plugin code should consume
  capabilities through views, not construct providers directly.

## Accepted Patterns

- Split identity and version: `api_id` plus `api_version { major, revision }`.
- Keep `fcl_api` transport-neutral and dependency-clean.
- Use one shared `fcl::api::error_payload`; transports must not invent duplicate
  error DTOs.
- Preserve native HTTP mapping; use `fcl::api::frame` only for
  message-oriented transports.
- Keep product DTO serialization beside DTO ownership through Boost.Describe and
  `FCL_DECLARE_SERIALIZATION`.
- Use typed exceptions with numeric category codes so local code can catch
  concrete types and remote clients can preserve stable identity.
- Track message-oriented calls by `call_id`: duplicate active ids, unknown ids
  and frames after terminal state are protocol errors.
- Keep frame kinds explicit: request, response, error, cancel, stream item and
  stream end. Streaming is a first-class API method kind, not an enum placeholder.
- Separate protocol-neutral API interceptors from transport-specific middleware:
  trace/authz/metrics/limits can run for every binding, while HTTP middleware
  remains HTTP-native.
- Treat builder options as contractual behavior. If a binding exposes codec,
  max inflight, deadline, peer policy, frame size or route middleware, it must
  enforce that option and have a test.
- API transport session admission is strand-owned, not atomic-only: stream slot
  reservation, release and drain wakeups must be serialized together. See
  `docs/runtime/thread-safety.md`.
- Keep transport owner plugins boring and narrow: they centralize node/server
  lifecycle and route mounting, while product plugins contribute handlers or
  typed API bindings through a safe local API.

## Rejected Patterns

- Reintroducing FC exception hierarchy or variant-backed serialized exception
  chains.
- A single generic `/rpc` HTTP endpoint for every API method.
- Per-transport duplicate error payload DTOs.
- Letting `fcl_api` import `fcl_app` or concrete transports.
- Macro-based API declaration sugar in this pass; explicit descriptor builders
  keep the first production surface reviewable.
- Decorative protocol builders that store `.deadline(...)`, `.backpressure(...)`
  or `.peer_policy(...)` without changing runtime behavior.
- Making HTTP consume `fcl::api::frame` as a universal body shape; normal HTTP
  clients must still be able to use native method/path/query/status semantics.

## Builder Ownership Boundary

- `fcl.http.api.binding` owns API route mapping, request/response codec, HTTP status
  projection, API error JSON and API middleware contributions. It does not own
  bind address, TLS certificates, server lifecycle or product auth policy.
- `fcl.websocket.api` owns frame codec checks, max frame size, max inflight calls
  and bidirectional API session dispatch. It does not own HTTP upgrade routing,
  TLS verification or reconnect policy.
- `fcl.quic.api` owns API frame dispatch over QUIC streams, call deadline,
  max concurrent API calls and stream lifecycle policy. It does not own ALPN,
  certificates, listener/connector setup or packet/transport limits.
- `fcl.p2p.api` owns protocol artifact creation, peer requirement checks, codec,
  per-peer max inflight calls and continuous API sessions. It does not own peer
  identity, relay, hole punching, peer store lifecycle or node bootstrap.
- `fcl.plugins.net.p2p.node` owns one P2P node lifecycle for an application and
  publishes a narrow contribution API. It does not replace `fcl.p2p.api`; it
  consumes built bindings/routes and mounts them centrally before startup.

## Tests

The focused gate is:

```sh
ctest --test-dir build/fcl-typed-exceptions-debug --output-on-failure \
  -R "^(test_fcl_exceptions|test_fcl_raw|test_fcl_api|test_fcl_app|test_fcl_http_websocket|test_fcl_quic_p2p|test_fcl_plugins)$" \
  --timeout 300
```
