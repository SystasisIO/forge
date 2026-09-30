# forge_api_core

`forge_api_core` is the neutral typed contract layer for local plugin-to-plugin calls
and API-over-transport bindings. It does not own HTTP, WebSocket, QUIC, P2P or
application lifecycle. It owns the contract vocabulary: API identity/version,
method descriptors, typed handles, registry/view/installer, canonical
message-oriented frames and the shared external error payload.

## When To Use

- A plugin or application needs to publish a typed C++ capability to consumers.
- A transport binding needs one contract shape for local and remote calls.
- A protocol needs stable API identity, major version and minimum revision
checks before invoking application handler code.
- Network errors must preserve typed exception identity without leaking internal
diagnostic context.

## When Not To Use

- Do not use `forge_api_core` as a replacement for `forge_net_http`, `forge_net_quic`,
  `forge_net_websocket` or `forge_net_p2p`.
- Do not put transport paths, peer policies, HTTP status routing or server
  lifecycle in the core API layer.
- Do not invent per-transport error DTOs. Use `forge::api::core::error_payload`.

## Public Modules

- `forge.api.core.types` — API ids, versions, refs, codec ids, call ids, method kinds,
  frame kinds, `frame` and `error_payload`.
- `forge.api.core.descriptor` — contract and method descriptors.
- `forge.api.core.error_projection` — error payload projection and remote typed-error restore.
- `forge.api.core.handle` — typed local/remote handle wrapper.
- `forge.api.core.stream_reader` and `forge.api.core.stream_writer` — move-only,
  incremental typed stream endpoints.
- `forge.api.core.duplex_stream` — independent typed input and output directions.
- `forge.api.core.call_options` — per-call limits and optional total deadline.
- `forge.api.core.server_stream_call`, `forge.api.core.client_stream_call` and
  `forge.api.core.bidirectional_stream_call` — caller-owned live calls with
  half-close, cancellation and mandatory terminal completion.
- `forge.api.core.registry` — registry, installer, view and local frame dispatch.
- `forge.api.core.binding` — binding plan and protocol-neutral interceptors.
- `forge.api.core.dispatcher` — shared API frame dispatcher for stream-oriented
  bindings.
- `forge.api.core.trusted_invocation` — immutable, non-wire values established
  by the server-side binding for one invocation.
- `forge.api.core.server_supplied` — opt-in typed request-field reset and
  server-side trusted-value application.
- `forge.api.core.exceptions` — core typed exceptions such as `method_not_found`,
  `incompatible_version` and `remote_internal`.

Target: `forge_api_core`.

## Local Contract

The default contract surface is local. Local calls use the C++ method directly,
so their request and response types do not need Boost.Describe, default
constructors or raw operators. Move-only domain values are valid local DTOs.

```cpp
#include <forge/api/core/macros.hpp>

class lookup_request {
 public:
   explicit lookup_request(std::string key);
   lookup_request(lookup_request&&) noexcept = default;
   lookup_request(const lookup_request&) = delete;
};

class cache : public forge::api::core::contract<cache> {
 public:
   virtual ~cache() = default;

   virtual boost::asio::awaitable<models::chunk>
   read(lookup_request request) = 0;
};

FORGE_API(cache, FORGE_API_CONTRACT("cache", 1, 8), FORGE_API_METHOD(read))
```

Raw bindings are generated only when the contract opts into
`surface::remote`. Remote DTO serialization stays beside the DTO owner:

```cpp
BOOST_DESCRIBE_STRUCT(protocol::read_chunk, (), (ref, offset, limit))
FORGE_DECLARE_SERIALIZATION(protocol::read_chunk)
```

For new APIs that are more naturally expressed as several C++ arguments,
`FORGE_API_METHOD(method, arg...)` records positional argument names while the
types are still deduced from the C++ method signature:

```cpp
class cache_api : public forge::api::core::contract<
   cache_api,
   forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~cache_api() = default;

   virtual boost::asio::awaitable<store_receipt>
   store_chunk(cache_name cache, chunk_ref ref, chunk_bytes bytes) = 0;
};

FORGE_API(
   cache_api,
   FORGE_API_CONTRACT("cache", 1, 0),
   FORGE_API_METHOD(store_chunk, cache, ref, bytes))

BOOST_DESCRIBE_STRUCT(chunk_ref, (), (digest, size))
BOOST_DESCRIBE_STRUCT(store_receipt, (), (stored, version))
```

The argument names are metadata, not type declarations. Existing
`FORGE_API_METHOD(read)` one-request DTO methods keep their old source and wire
shape. Positional methods are new declarations and use an internal argument-pack
payload for frame transports. HTTP-specific request wrappers are not part of
`forge_api_core`; `forge_api_http` supports them as fields of described request DTOs and
keeps HTTP positional methods limited to path/query routing plus an optional
single JSON DTO body.

If a C++ interface has overloads or local convenience helpers with the same
method name, use the typed method macro to select the wire method explicitly:

```cpp
struct sign_request {
   std::string key_id;
   forge::crypto::digest::sha256 digest;
};

struct sign_response {
   std::vector<std::uint8_t> signature;
};

class signature_api : public forge::api::core::contract<signature_api> {
 public:
   virtual ~signature_api() = default;

   virtual boost::asio::awaitable<sign_response>
   sign(sign_request request) = 0;

   boost::asio::awaitable<sign_response>
   sign(std::string key_id, forge::crypto::digest::sha256 digest);
};

FORGE_API(signature_api,
        FORGE_API_CONTRACT("signature", 1, 0),
        FORGE_API_METHOD_TYPED(sign, sign_request, sign_response))
```

`FORGE_API_METHOD_TYPED_SINCE(...)`,
`FORGE_API_METHOD_TYPED_DEPRECATED(...)` and
`FORGE_API_METHOD_TYPED_DEPRECATED_SINCE(...)` provide the same revision and
deprecation metadata for overloaded methods.

## Publish And Consume In Process

```cpp
boost::asio::awaitable<void>
application::on_provide(forge::app::application_context& context) {
   context.apis().install<cache>(std::make_shared<cache_impl>());
   co_return;
}

boost::asio::awaitable<void>
consumer_plugin::initialize(forge::app::plugin_context& context) {
   cache_ = context.apis().get<cache>({.id = {"cache"}, .major = 1, .min_revision = 8});
   auto chunk = co_await cache_->read(protocol::read_chunk{.ref = ref});
}
```

## Message-Oriented Frame

WebSocket, QUIC, P2P and TCP-like bindings use `forge::api::core::frame`.
Ordinary unary HTTP routes keep their native request/response mapping; live
HTTP/1.1 stream bodies use a versioned length-delimited form of the same frame.

```cpp
auto request = forge::api::core::frame{
   .kind = forge::api::core::frame_kind::request,
   .id = {.value = 42},
   .api = {.id = {"cache"}, .major = 1, .min_revision = 8},
   .method = "read",
   .codec = {.value = "forge.raw"},
};
forge::raw::pack(request.payload, protocol::read_chunk{.ref = ref});
```

Frame lifecycle belongs to the wire-v2 session in `forge_api_stream`. Call id
zero is reserved for session control. `stream_end` closes only one direction;
the call itself terminates exactly once with `response`, `error` or `cancel`.
The session enforces deadlines, inflight limits, flow-control windows and late
frame tombstones before dispatching application code.

Descriptor method kinds are inferred from the final move-only endpoint:

```cpp
class cache : public forge::api::core::contract<
   cache,
   forge::api::core::surface::local |
      forge::api::core::surface::remote> {
 public:
   virtual boost::asio::awaitable<void>
   watch(watch_request, forge::api::core::stream_writer<chunk>) = 0;

   virtual boost::asio::awaitable<put_result>
   upload(put_request, forge::api::core::stream_reader<chunk>) = 0;

   virtual boost::asio::awaitable<void>
   exchange(session_request,
            forge::api::core::duplex_stream<request, response>) = 0;
};

FORGE_API(cache,
          FORGE_API_CONTRACT("cache.events", 2, 0),
          FORGE_API_METHOD(watch, request),
          FORGE_API_METHOD(upload, request),
          FORGE_API_METHOD(exchange, request))
```

The endpoint is always the final parameter and is not a wire argument. Zero or
more ordinary arguments may precede it. `std::vector<T>` without a stream
endpoint remains a unary DTO. For overloaded methods, use
`FORGE_API_METHOD_EXACT` with the exact member pointer.

```cpp
auto call = co_await handle.async_open<&cache::upload>(request);
co_await call.async_write(first);
co_await call.async_write(second);
co_await call.async_close();
auto result = co_await call.async_finish();
```

Destroying an unfinished call cancels that call only. A call keeps its
implementation or remote invoker alive independently of the source handle.

## API Over Transport

`forge.api.stream` is the reusable binding for API-over-stream serving. It
sits above `forge_api_core` and `forge_net_transport`, uses
`forge::net::transport::stream`, and owns the shared server frame read/write
loop, codec checks, max-inflight limits, deadlines and error projection.
`forge.api.transport` builds the generic client, connection and session layer
on top of that stream primitive.

This layer must not move into `forge_net_transport`: transport stays a low-level
byte-stream/session contract and must not import the API contract layer.
`forge.api.quic.binding` and `forge.api.p2p.binding` are thin adapters or policy
wrappers over the API stream runtime. WebSocket preserves message boundaries by
mapping exactly one binary WebSocket message to one transport frame, then uses
the same wire-v2 session. HTTP remains a separate binding because it is
request/response oriented rather than a long-lived bidirectional stream.

The network/P2P implementation order is tracked only in
[`docs/network/quic-p2p.md`](../../../docs/network/quic-p2p.md); this README only
records the API-layer boundary.

## Interceptors

Interceptors are protocol-neutral API middleware. Use them for tracing,
authorization decisions, metrics and limits that should behave the same over
WebSocket, QUIC, P2P or an in-process test binding.

```cpp
auto plan = forge::api::core::binding()
   .serve(app.apis())
   .interceptor(forge::api::core::interceptor()
      .id("authz")
      .phase(forge::api::core::interceptor_phase::authorize)
      .order(10)
      .handler([](forge::api::core::call_context& call) -> boost::asio::awaitable<void> {
         co_await authorize_api_call(call.api, call.method, call.meta);
      })
      .build())
   .build();
```

HTTP-specific request middleware stays in `forge_net_http` or the `forge::plugins::net::http::server`
plugin facade; API interceptors do not parse HTTP headers, routes or upgrade
state.

## Error Payload

Typed FORGE exceptions are projected to one shared DTO:

```json
{
  "error": "chunk_not_found",
  "message": "chunk not found",
  "retryable": false,
  "identity": {
    "category": "cache",
    "code": 1
  }
}
```

`identity` is stable machine-readable exception identity. Internal capture
context is diagnostic-only and is not returned externally by default.

Known remote errors can be restored to typed exceptions through the same method
descriptor:

```cpp
const auto payload = forge::raw::unpack<forge::api::core::error_payload>(frame.payload);
const auto* method = forge::api::core::find_method(cache::describe(), frame.method);

try {
   forge::api::core::raise_remote_error(payload, method);
} catch (const cache_errors::chunk_not_found& error) {
   // Handle the same typed exception shape as local plugin calls.
}
```

Unknown remote identities become `forge::api::core::exceptions::remote_internal` with
the remote category/code preserved as redacted-safe context.

## Trusted Invocation

`trusted_invocation` carries immutable server authority by exact C++ type. It
is deliberately non-serializable and is distinct from frame metadata: metadata
continues through the existing boundary and interceptor path, while trusted
values are available only when dispatch enriches typed fixed request arguments.

Specialize `server_supplied<T>` with `required`, `reset(T&)` and
`apply(T&, const trusted_invocation&)`. Generated remote calls reset only their
owned wire copies. Contextual server dispatch decodes fixed arguments once,
resets them again, applies trusted values, and returns
`server_supplied_unavailable` before the handler when a required value is
missing. Traversal is exact-specialization first, then fixed tuple-like values
and Boost.Describe structs; it does not implicitly traverse optionals, ranges,
variants or pointers. Stream items are never enriched.

The ordinary dispatch and legacy descriptor invokers remain unchanged: they do
not require trusted values and continue to serve existing APIs. Trusted metadata
is still available to interceptors for compatibility, but it never creates typed
authority. Core does not name or depend on a transport or P2P identity type;
transport adapters own the conversion from their authenticated connection state.

## Risks And Anti-Patterns

- Do not branch on exception message strings. Use typed exceptions or
  `identity.category/code`.
- Do not silently choose the first API implementation when version checks fail.
- Do not expose stack traces, secrets or capture context in network payloads.
- Do not force HTTP into a frame-only POST RPC shape; use native HTTP mapping in
  `forge.api.http.binding`.
- Do not add a builder option that only stores a flag. Any option exposed by API
  bindings must change behavior and have a test.

## Tests

`test_forge_api_core` covers descriptor validation, local registry/view lookup, raw
frame dispatch, trusted invocation enrichment, shared error payload serialization,
declared typed exception projection and typed remote exception restoration.
