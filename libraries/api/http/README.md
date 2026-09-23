# forge_api_http

`forge_api_http` binds `FORGE_API(...)` contracts to native HTTP routes. It sits
above `forge_net_http` and `forge_api_core`: the lower layer owns HTTP mechanics, while
this library owns typed request/response mapping, codec dispatch and typed
clients.

## When To Use

- Expose a described Forge API contract over HTTP route/path/status semantics.
- Build route-aware typed clients for the same contract.
- Expose server-stream and client-stream methods as incremental HTTP/1.1
  response and request bodies.
- Use JSON by default and opt into XML per route for typed DTO bodies.
- Keep native HTTP escape hatches such as file, stream, bytes, empty,
  body-stream, body-bytes and multipart routes.

## When Not To Use

- Do not use `forge_api_http` for raw streaming endpoints that do not have a
  typed API contract. Use `forge_net_http` directly.
- Do not put server bind, TLS, socket lifecycle or application auth policy in
  this library. Those belong to `forge_net_http`, plugins or consumers.
- Do not force every API into one generic RPC endpoint. Use explicit HTTP
  routes and statuses.

## Public Modules

- `forge.api.http.parameters` — HTTP request DTO wrapper parameters and special
  response types in namespace `forge::api::http`; transport primitives remain
  in `forge::net::http`.
- `forge.api.http.mapping` — route metadata, `traits<T>`, route template parsing
  and rendering metadata.
- `forge.api.http.binding` — `binding_builder`, `binding_plan`, `binding()` and
  server-side mount.
- `forge.api.http.client_request` — typed proxy request construction internals.
- `forge.api.http.client_response` — typed proxy response materialization and
  bounded error decode internals.
- `forge.api.http.proxy` — `remote<T>()` and remote invoker glue.
- `forge.api.http.openapi` — OpenAPI 3.1 generation from the same typed contract
  and route descriptors used by the HTTP binding.

Macro header: `<forge/api/http/macros.hpp>`.

`forge::api::http::openapi_document_builder` aggregates those existing
`FORGE_HTTP_API(...)` descriptors without introducing a second routing DSL.
`add<Interface>(base_path)` prefixes its paths, while `build(info)` emits a
deterministic document. Duplicate verb/path pairs and reused operation IDs with
incompatible request or response schemas are rejected as typed protocol errors.

## Live Streaming

HTTP/1.1 supports server-stream and client-stream methods. Server streams use a
chunked response; client streams use a streamed request body. Both carry
versioned, length-delimited Forge API frames with media type
`application/vnd.forge.api-stream; version=2`. Transport body backpressure is
the flow-control mechanism, so HTTP bodies do not carry `stream_window` frames.

The endpoint is still the final C++ parameter and is not part of the fixed wire
arguments. For client streams, fixed arguments may be mapped only to path,
query, header or cookie fields because the request body belongs to stream
items. A conflicting body/form/body-stream mapping is rejected while building
the binding. HTTP/1.1 bidirectional methods are rejected both at binding
construction and before client I/O; use WebSocket, P2P or QUIC for duplex calls.

If a server-stream failure occurs before response headers, the binding returns
the ordinary HTTP error response. After streaming starts, it emits a terminal
API error frame. Decoders bound frame length before allocation, require wire
major 2, reject trailing bytes and require exactly one terminal outcome.

## Target And Component

- CMake target: `forge_api_http`
- Package target: `Forge::forge_api_http`
- Package component: `api_http`

## Dependencies

- `forge_net_http`
- `forge_api_core`
- `forge_codec_json`
- `forge_codec_xml`
- `forge_raw`
- `forge_reflect`
- `forge_schema`
- Boost.Asio

## Examples

### Bind A Contract

```cpp
#include <forge/api/http/macros.hpp>

import forge.api.core.binding;
import forge.api.http.binding;
import forge.api.http.proxy;

auto local = forge::api::core::binding().serve(registry).build();
auto http_binding = forge::api::http::binding()
   .use(std::move(local))
   .bind<catalog_api>()
   .build();

auto remote = co_await forge::api::http::remote<catalog_api>(client);
```

### Select XML For A DTO Route

Routes use JSON request, response and error bodies by default. Per-route codec
options can opt into XML for typed DTO bodies while preserving native response
bypasses.

```cpp
FORGE_HTTP_API(catalog_api,
   FORGE_HTTP_PUT(update_item, "/items/:id", ok,
      FORGE_HTTP_REQUEST_BODY(xml),
      FORGE_HTTP_RESPONSE_BODY(xml),
      FORGE_HTTP_ERROR_BODY(xml)))
```

`forge_api_http` accepts a missing `Content-Type` as the route's configured
request codec for compatibility with existing JSON routes. Explicit mismatches
still fail with `415 Unsupported Media Type`. Typed DTO responses check `Accept`
before invoking the handler when the emitted codec is not acceptable.

## Security And Boundaries

- A positional argument whose exact type specializes
  `forge::api::core::server_supplied<T>` is never an HTTP body, path, query,
  header, cookie or form field. The client omits it; the server constructs a
  default value and applies the trusted invocation before dispatch. A required
  value without trusted transport identity fails closed. Explicit route
  bindings for that argument are rejected when mounted, and OpenAPI omits it.
  A normal request DTO containing a server-supplied member remains a DTO;
  its member is still reset and replaced by the core dispatch boundary.
- JSON/XML DTO codecs apply only to typed DTO bodies. `file_response`,
  `streaming_response`, `stream_response`, `bytes_response`, `empty_response`,
  `body_stream`, `body_bytes` and multipart/form-data bypass DTO codecs.
- Error bodies use the route error codec and the shared Forge API error payload
  shape.
- Do not log request bodies, headers or query strings before redaction.
- Keep protocol-specific error names, signing, authorization and storage policy
  outside this library.

## Common Mistakes

- Do not add route options that do not change runtime behavior and tests.
- Do not decode native stream/file/bytes responses through JSON or XML.
- Do not buffer API stream items into a vector or emulate bidirectional HTTP by
  delaying either direction.
- Do not make `forge_net_http` depend on `forge_api_http`; dependency direction is
  `forge_api_http -> forge_net_http`.
- Do not use positional HTTP arguments for large envelopes. Use a described
  request DTO when the body or validation surface grows.

## Tests

- `test_forge_api_core`
- `test_forge_http_websocket`
- `test_forge_package_api_http_component`
