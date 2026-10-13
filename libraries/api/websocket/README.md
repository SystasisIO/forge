# Forge API WebSocket

`forge_api_websocket` binds one already-upgraded WebSocket connection to one
Forge API Live Streaming wire-v2 session.

## Public Modules

- `forge.api.websocket.binding` serves a `forge::api::core::binding_plan`.
- `forge.api.websocket.connection` is a client-side
  `forge::api::core::connection`.
- `forge.api.websocket.stream` adapts WebSocket messages to
  `forge::net::transport::stream` chunks.

The binding supports unary, server-stream, client-stream and bidirectional-stream
methods. `forge_api_stream` owns the mandatory v2 hello, call IDs, flow-control
windows, cancellation, bounded call queues and session/call lifetime. There is no
WebSocket-specific frame codec and no vector or batch dispatcher fallback.

## Server

```cpp
auto plan = forge::api::core::binding()
   .serve(app.apis())
   .export_api<cache>(cache::ref())
   .build();

auto binding = forge::api::websocket::api()
   .use(std::move(plan))
   .backpressure({
      .max_inflight = 128,
      .max_buffered_bytes = 16U * 1024U * 1024U,
   })
   .build();

router.websocket_guarded("/api", binding.guarded(admit_websocket_route));
```

`guarded(provider)` adapts the existing HTTP `websocket_guarded` seam. The async
provider returns `forge::api::core::invocation_context` and owns credential,
Origin/CSRF and product policy. An absent/invalid subject is rejected before 101.
TLS caller composition rejects duplicate trusted types. The accepted callback
captures owned immutable context, never the request or raw bearer, and passes it
to the existing `serve_stream` session. Do not also register an ordinary WebSocket
route at the protected path. Provider denial responses contain no backend text.

Expiry is bounded by the subject's `valid_until()`. A positive recheck interval
and timeout allow earlier revoke; expiry/revoke cancel the session and active calls,
then close the socket. This affects transport admission, not the fate of previously
accepted product work. Cancellation must be cooperative; blocking hooks are not
supported. The old `accept(socket)` remains for transport-only owners, while
`accept(socket, context)` supports explicit trusted admission. Both reuse one wire-v2
session implementation; required subjects cannot be supplied by client frames.

## Client

```cpp
auto socket = co_await websocket_client.async_connect("/api");
auto connection = forge::api::websocket::connection{std::move(socket)};
auto cache = co_await connection.get_remote_api<cache_api>();
auto subscription = co_await cache.async_open<&cache_api::subscribe>();
```

All calls opened from the connection share one incremental API session. Closing
or cancelling the connection delegates terminal behavior to `forge_api_stream`
and closes the underlying WebSocket transport.

## Buffering

Each API wire frame is one WebSocket message. The adapter bounds queued inbound
and outbound message bytes with `max_buffered_bytes`; `forge_api_stream` applies
its stricter per-frame, per-item, flow-window and aggregate call limits before
application handlers consume or produce stream items. The buffer limit must fit
one configured frame plus its small transport length prefix.

## Ownership

- `forge_net_websocket` owns WebSocket handshake, message I/O, ping and close.
- `forge_net_transport` owns the byte-stream concept consumed by API sessions.
- `forge_api_stream` owns wire-v2 framing and session semantics.
- `forge_api_websocket` owns only the message-to-stream adapter and WebSocket
  client/server binding.
