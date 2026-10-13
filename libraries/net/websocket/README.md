# forge_net_websocket

`forge_net_websocket` provides WebSocket client and connection primitives. Server-side
entry is intentionally through `forge_net_http` WebSocket upgrade routes so HTTP and
WebSocket share routing, TLS and lifecycle boundaries.

## When To Use

- A component needs bidirectional message streams over an HTTP-origin service.
- You need a reusable WebSocket connection abstraction with serialized writes.
- You need TLS WebSocket client support over the same runtime model as HTTP.

## When Not To Use

- Do not create application subscription/event semantics here.
- Do not introduce a standalone WebSocket server in v1; HTTP owns upgrade.
- Do not use WebSocket messages as an implicit authorization boundary.

## Public Modules

- `forge.net.websocket.connection` — connection pointer, reads/writes, close behavior.
- `forge.net.websocket.client` — client endpoint/options and connect helpers.

Target: `forge_net_websocket`.

Dependencies: `forge_asio`, `forge_net_tls`, Boost.Asio, Boost.Beast, OpenSSL.

When a TLS WebSocket is upgraded by `forge_net_http`, the connection retains the
accepted `forge_net_tls` snapshot for its full lifetime. Rotating the HTTP
server's provider therefore affects only later TLS connections.

`connection::accept(request, buffered_input)` transfers owned bytes already read
after the HTTP request into a private Beast `buffered_read_stream`. The request
remains borrowed until acceptance completes; the input string is owned by the
coroutine and copied once into the connection's buffer. These bytes are consumed
before later socket reads, including input larger than Beast's 1536-byte handshake
buffer. The HTTP server bounds this input with its existing request buffer limit
and waits for its admission monitor to stop before handing off the stream.
Direct callers must bound their input and stop every prior stream read before
accepting. The existing `accept(request)` overload remains available.

## Examples

### Connect A Client

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.websocket.client;
import forge.net.websocket.connection;

auto endpoint = forge::net::websocket::client_endpoint{
   .host = "127.0.0.1",
   .port = "8443",
   .base_path = "/api",
   .tls = true,
};

auto client = forge::net::websocket::client{runtime, endpoint};
boost::asio::awaitable<void> connect_events(forge::net::websocket::client& client) {
   forge::net::websocket::connection::ptr connection = co_await client.async_connect("/events");
   use_connection(std::move(connection));
}
```

### Build A Target Path

```cpp
auto target = endpoint.make_target("events"); // "/api/events"
```

### HTTP Upgrade Route

```cpp
import forge.net.http.router;

router.websocket("/events", [](std::shared_ptr<forge::net::websocket::connection> connection) {
   connection->on_message([](forge::net::websocket::connection& ws, std::string message)
      -> boost::asio::awaitable<void> {
      co_await ws.send(std::move(message));
   });
   // forge::net::http::server starts the WebSocket read loop after this callback.
});
```

### Serve An API Session

`forge.api.websocket.binding` uses `forge::api::core::frame` because WebSocket is
message-oriented and bidirectional. The binding is continuous: every inbound
WebSocket message is decoded as an API frame, checked against the configured
codec and frame-size limit, dispatched through `forge::api::core::frame_dispatcher`,
then replied with a response/error frame.

```cpp
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.websocket.binding;

auto plan = forge::api::core::binding()
   .serve(app.apis())
   .export_api<cache>({.id = {"cache"}, .major = 1, .min_revision = 8})
   .require_peer_api<client_session>({.id = {"client.session"}, .major = 1})
   .build();

auto binding = forge::api::websocket::api()
   .use(plan)
   .codec({"forge.raw"})
   .max_frame_size(1 << 20)
   .backpressure({.max_inflight = 128})
   .build();

router.websocket("/api", [binding](forge::net::websocket::connection::ptr connection) mutable {
   boost::asio::co_spawn(
      app.runtime().context(),
      binding.accept(std::move(connection)),
      boost::asio::detached);
});
```

`forge.api.websocket.binding` owns API-level WebSocket binding behavior only:
max-frame-size rejection and handoff to the shared `frame_dispatcher`. It does
not use `forge.api.transport`, because WebSocket messages are not
`transport::stream` chunks. HTTP upgrade routes, TLS verification and application
reconnect policy stay with the transport owner.

### Send, Ping And Close

```cpp
import forge.net.websocket.connection;

boost::asio::awaitable<void> send_healthcheck(forge::net::websocket::connection::ptr connection) {
   co_await connection->send(R"({"type":"hello"})");
   co_await connection->ping("health");
   auto metrics = connection->metrics();
   record_metrics(metrics);
   co_await connection->close();
}
```

### Observe Close

```cpp
connection->on_close([](forge::net::websocket::connection& ws) {
   auto metrics = ws.metrics();
   record_disconnect(metrics.close_count);
});
```

## Security Notes

`client_options::verify_peer` defaults to `true`. Test-only insecure modes must
stay explicit and should not be hidden behind broad "dev" defaults.

## Risks And Anti-Patterns

- Do not treat a connected WebSocket as an authenticated session by itself.
  Application auth and replay rules live above the connection.
- Do not send concurrent writes through ad-hoc caller code if the connection
  does not serialize them. Use the FORGE connection API boundary.
- Do not put bearer tokens in query strings for convenience; they commonly leak
  through logs, metrics and diagnostics.
- Do not leave message handler exceptions to `co_spawn(..., detached)` with an
  empty completion handler. FORGE records handler failures and closes the handler
  path instead of silently swallowing correctness bugs.
- Do not invent WebSocket-specific error payloads for typed APIs; use
  `forge::api::core::error_payload` in error frames.
- Do not treat `.backpressure(...)` as a decorative value. If max inflight is
  exceeded, the API call runtime rejects the frame before application handlers run.
- Do not put HTTP upgrade or TLS policy in `forge.api.websocket.binding`; it is an API
  binding over an already accepted connection.

## Typical Mistakes

- Do not perform concurrent writes directly against a connection unless the
  connection API serializes them.
- Do not leak bearer tokens in query strings when rendering endpoint diagnostics.
- Do not assume WebSocket reconnection is automatic application behavior; callers own
  policy.
- Do not make message handlers mutate shared state without a strand/serialization
  rule in the caller.

## Tests

`test_forge_http_websocket` covers WebSocket echo over the HTTP server port and TLS
client connection behavior.
