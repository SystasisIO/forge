# forge_net_http

`forge_net_http` is the HTTP substrate: URL parsing, FORGE-owned request/response
messages, cookie handling, bounded static assets, streaming body primitives,
routing, middleware, server and client/connection primitives. It uses Boost.Beast/URL internally but keeps
FORGE-owned public message, route and lifecycle semantics.

Application-level server lifecycle can be owned directly with `forge::net::http::server`
or composed through the official `forge.plugins.http.server` plugin. The library
still owns HTTP mechanics; the plugin owns app lifecycle/config composition.

## When To Use

- Build local or service HTTP APIs over Boost.Asio.
- Share routing and middleware with WebSocket upgrade handling.
- Use a queued per-connection HTTP client for serialized requests.
- Publish HTTP-native stream routes when request/response bodies should not be
  forced through a full in-memory DTO path.

## When Not To Use

- Do not put application DTOs or JSON contracts in this library.
- Do not use HTTP as a security boundary by itself; auth belongs to consumers.
- Do not add a central application request queue here; request ownership remains
  at server/router/connection boundaries.

## Public Modules

- `forge.net.http.types` — FORGE-owned Beast-like request/response wrappers, HTTP
  method/status enums and endpoint DTO state.
- `forge.net.http.body`, `forge.net.http.stream` — FORGE-owned chunk, reader, writer and
  stream route types.
- `forge.net.http.file`, `forge.net.http.range` — file responses, static roots and byte
  range parsing.
- `forge.net.http.cookie` — strict `Cookie` and `Set-Cookie` parsing, formatting and
  repeated response header emission.
- `forge.net.http.assets` — bounded GET/HEAD asset mounts over hardened static-file
  streaming, MIME allow-listing and SPA fallback.
- `forge.net.http.negotiation` — generic media type and `Accept` header helpers for
  libraries that choose their own codecs above `forge_net_http`.
- `forge.net.http.upload` — upload reader, spill-to-disk spool and multipart form-data
  parsing.
- `forge.net.http.base_url`, `forge.net.http.target`.
- `forge.net.http.router`, `forge.net.http.route_context`, `forge.net.http.middleware`.
- `forge.net.http.client`, `forge.net.http.connection`, `forge.net.http.server`.

Target: `forge_net_http`.

Dependencies: `forge_asio`, `forge_net_tls`, `forge_net_websocket`, `forge_codec_json`,
`forge_schema`, Boost.Asio, Boost.Beast and Boost.URL.

HTTPS client connections acquire their OpenSSL context from `forge_net_tls`; the
client keeps that immutable snapshot for the lifetime of its TLS connection.
The TLS leaf defaults to TLS 1.3 only, while this established HTTPS client
explicitly keeps OpenSSL's system-default protocol range to preserve its prior
compatibility behavior. SNI and hostname verification remain separate: the
client configures both for each HTTPS connection.

`forge::net::http::client_options` extends that existing TLS context/provider:
the default provider uses system trust, while a caller may supply a provider
with a client certificate for mTLS, an explicit SNI name, an expected hostname,
and an optional SHA-256 leaf certificate pin. HTTPS always requires
`verify_peer`; a client cannot disable certificate verification. Its optional
per-request header provider is bounded and cannot set `Host`, framing, or
hop-by-hop headers, so request ownership and transport framing remain with the
client. The provider runs synchronously before request enqueueing, must be
nonblocking and thread-safe, and has no cancellation channel. Its output is
rejected when a header name collides case-insensitively with the request or with
another provider header; this includes `Authorization`. Its execution consumes
the request timeout budget before transport work begins.
`validate_provider_headers` exposes that exact policy for components that must
reject a resolved provider result during startup instead of on the first request.

`forge::net::http::server` can instead receive a shared
`forge::net::tls::context_provider`. Listener mode is fixed when the server
starts: a TLS listener completes a TLS 1.3 server handshake before parsing any
HTTP bytes, has a positive handshake timeout and a bounded pending-handshake
count, and never falls back to plaintext. One provider snapshot is acquired for
each accepted TLS connection and remains alive through HTTP and WebSocket
upgrade lifetimes; `context_provider::replace()` affects only later accepts.
Server mTLS uses `peer_verification::require_peer_certificate` with configured
trust anchors. The library takes PEM material, never plugin Secret IDs.

Boost.Beast remains the runtime donor and backend for parser/serializer/socket
mechanics, but public HTTP APIs use `forge::net::http::request` and
`forge::net::http::response` wrappers rather than Beast message aliases.

Typed `FORGE_HTTP_API(...)` route binding lives in the separate `forge_api_http`
target/component. Its public modules are `forge.api.http.binding`,
`forge.api.http.mapping` and `forge.api.http.proxy`; its macro header is
`<forge/api/http/macros.hpp>`.

## Examples

### Parse Base URL

```cpp
import forge.net.http.base_url;

auto endpoint = forge::net::http::parse_base_url("https://127.0.0.1:8443/api");
auto target = endpoint.make_target("/healthz"); // "/api/healthz"
```

### Parse A Request Target

```cpp
import forge.net.http.target;

auto parsed = forge::net::http::parse_target("/v1/items?limit=10&cursor=abc");
auto first_segment = parsed.segments.front(); // "v1"
auto query = parsed.query_params.front();
```

### Route Requests

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.http.router;
import forge.net.http.types;
import forge.net.http.cookie;

auto router = forge::net::http::router{};
router.get("/healthz", [](forge::net::http::route_context& ctx)
   -> boost::asio::awaitable<forge::net::http::response> {
   co_return forge::net::http::make_text_response(ctx.request, forge::net::http::status::ok, "ok");
});
```

### Use Endpoint Request State

Typed HTTP request DTOs may derive from `forge::net::http::endpoint_request` when a
handler needs read-only access to the incoming HTTP request or wants to add
response metadata. The base is not described with Boost.Describe and is ignored
by JSON/schema binding.

```cpp
struct read_request : forge::net::http::endpoint_request {
   std::string ref;
};

BOOST_DESCRIBE_STRUCT(read_request, (), (ref))

boost::asio::awaitable<chunk>
cache_impl::read(read_request request) {
   auto trace = request.request().header("X-Trace").value_or("");
   request.response().set("Cache-Control", "public, max-age=60");
   forge::net::http::append_set_cookie(
      request.response(), forge::net::http::set_cookie{.name = "trace", .value = std::string{trace}});
   co_return load_chunk(request.ref);
}
```

### Route Streaming Bodies

Use stream routes for upload/download mechanics that should be visible as HTTP
body flow, not as `FORGE_API` DTO calls. The server routes after headers are read;
middleware can reject before the body is consumed. For requests with
`Expect: 100-continue`, `forge_net_http` sends the interim `100 Continue` response
only when the route actually starts reading the body, so header-only rejection
does not force a large upload.

```cpp
import forge.net.http.body;
import forge.net.http.router;
import forge.net.http.stream;

router.post_stream("/upload", [](forge::net::http::stream_request& req)
   -> boost::asio::awaitable<forge::net::http::stream_response> {
   std::uint64_t received = 0;
   while (auto chunk = co_await req.body.async_read()) {
      received += chunk->bytes.size();
      consume_upload_chunk(chunk->bytes);
   }

   co_return forge::net::http::stream_response::buffered(
      forge::net::http::make_text_response(req.context.request, forge::net::http::status::ok, "stored"));
});

router.get_stream("/download", [](forge::net::http::stream_request& req)
   -> boost::asio::awaitable<forge::net::http::stream_response> {
   auto source = open_chunk_source(req.context.request);
   auto head = forge::net::http::response{forge::net::http::status::ok, req.context.request.version()};
   head.set(forge::net::http::field::content_type, "application/octet-stream");

   co_return forge::net::http::stream_response{
      .head = std::move(head),
      .body = [source = std::move(source)]() mutable
         -> boost::asio::awaitable<std::optional<forge::api::http::body_chunk>> {
         co_return co_await source.next_chunk();
      },
   };
});
```

Stream routes provide FORGE-owned body readers and response body sources. Use
`forge.net.http.upload` when the request body should be bounded, optionally spooled to
disk, or parsed as browser-style `multipart/form-data`.

### Negotiate Content Types

`forge.net.http.negotiation` parses media types and `Accept` headers generically.
It understands parameters, structured suffixes such as `+json`, wildcard media
ranges and `q=0` exclusion. Codec ownership stays outside `forge_net_http`; callers
provide the media types they support.

```cpp
import forge.net.http.negotiation;

constexpr auto xml = std::array{
   forge::net::http::media_type_match{.type = "application/xml", .structured_suffix = "+xml"},
   forge::net::http::media_type_match{.type = "text/xml"},
};

auto content_ok = forge::net::http::media_type_matches("application/custom+xml", xml);
auto accept_ok = forge::net::http::accept_allows("application/json;q=0, application/xml;q=1", xml);
```

### Read Uploads

`upload_reader` consumes a `body_reader` incrementally. Small payloads stay in
memory; larger payloads spill to an owner-private temporary file that is removed
when the returned `upload_part` is destroyed unless the caller explicitly
releases the spool.

```cpp
import forge.net.http.upload;

router.post_stream("/upload", [](forge::net::http::stream_request& req)
   -> boost::asio::awaitable<forge::net::http::stream_response> {
   auto reader = forge::net::http::upload_reader{
      std::move(req.body),
         forge::net::http::upload_options{
            .memory_threshold_bytes = 1 * 1024 * 1024,
            .max_file_bytes = 64 * 1024 * 1024,
            .max_field_bytes = 1 * 1024 * 1024,
            .max_total_bytes = 128 * 1024 * 1024,
         },
   };

   auto part = co_await reader.async_read();
   consume_upload(part);

   co_return forge::net::http::stream_response::buffered(
      forge::net::http::make_text_response(req.context.request, forge::net::http::status::ok, "stored"));
});
```

`async_read_multipart(content_type)` parses browser-style form uploads into
fields and file parts. It is not a domain-specific multi-step transfer workflow;
application state machines belong above `forge_net_http`.

Multipart limits are separate: `max_total_bytes` bounds the whole envelope,
`max_file_bytes` bounds each file part, and `max_field_bytes` bounds each
non-file field. `upload_part::filename` is untrusted client metadata; use
`safe_filename()` as a conservative basename or apply a stricter product policy
before using any uploaded name in a filesystem path.

### Serve Static Files And Ranges

`static_file_root` serves files through the stream response path with
descriptor-relative opens, opened-file `fstat`, traversal rejection, `HEAD`,
byte ranges and conditional metadata headers. `file_options::symlinks` defaults
to `symlink_policy::reject`; `follow` is an explicit legacy/general file-serving
mode, remains contained under a `static_file_root`, and must not be used for
untrusted browser assets. Request-time path resolution, descriptor opens,
`fstat`, and body `pread` calls run on a caller-owned bounded
`forge::asio::compute::pool`; the HTTP runtime worker only awaits completion.
The pool owner must outlive every root and path-backed response that uses its
executor, then stop or drain the pool as part of application shutdown.

```cpp
import forge.asio.compute;
import forge.net.http.file;
import forge.net.http.router;
import forge.net.http.stream;

auto file_reads = forge::asio::compute::pool{forge::asio::compute::pool::options{
   .worker_threads = 2,
   .max_pending_tasks = 64,
   .max_waiting_submissions = 64,
   .thread_name = "forge-http-file",
}};
auto files = std::make_shared<forge::net::http::static_file_root>(
   "/srv/public",
   file_reads.get_executor(),
   forge::net::http::file_options{
      .content_type = "application/octet-stream",
      .max_file_bytes = 64 * 1024 * 1024,
   });

router.get_stream("/files/:name", [files](forge::net::http::stream_request& req)
   -> boost::asio::awaitable<forge::net::http::stream_response> {
   co_return co_await files->serve(req, *req.context.route_param("name"));
});

router.head_stream("/files/:name", [files](forge::net::http::stream_request& req)
   -> boost::asio::awaitable<forge::net::http::stream_response> {
   co_return co_await files->serve(req, *req.context.route_param("name"));
});
```

This is a file-serving foundation, not a storage product. Object metadata,
authorization, placement and compatibility-specific error shapes belong above
`forge_net_http`.

### Mount Browser Assets

`asset_bundle` is the browser-facing owner above `static_file_root`. It only
serves `GET` and `HEAD`, rejects raw or percent-decoded traversal, separators,
NUL and control bytes, permits a small MIME set, and preserves file streaming,
ranges and ETags. Fingerprinted names receive immutable caching while
`index.html` stays `no-cache`. It always uses `symlink_policy::reject`. SPA
fallback applies only to extensionless asset paths after a static miss.
The low-level router takes the caller-owned compute executor explicitly; one
bounded pool may be shared across a bounded set of mounts.

```cpp
import forge.asio.compute;
import forge.net.http.assets;
import forge.net.http.router;

auto asset_reads = forge::asio::compute::pool{forge::asio::compute::pool::options{
   .worker_threads = 2,
   .max_pending_tasks = 64,
   .max_waiting_submissions = 64,
   .thread_name = "forge-http-assets",
}};
auto router = forge::net::http::router{};
router.mount_assets(
   forge::net::http::asset_mount{
      .path = "/admin",
      .root = "/srv/admin-ui",
      .index = "index.html",
      .spa_fallback = true,
      .max_file_bytes = 16 * 1024 * 1024,
   },
   asset_reads.get_executor());
```

Asset mounts cannot overlap each other or a reserved typed API prefix, except
that a root (`/`) SPA mount may coexist with reserved API prefixes. A normal
route always wins over an asset request, and a reserved API miss remains a 404
rather than falling through to the SPA. This permits a root SPA with
`/api/v1`: exact API routes run first, unknown `/api/v1/*` stays an API 404,
and only `GET`/`HEAD` requests outside the reserved API namespace may use asset
fallback.

### Mount API Bindings

`FORGE_HTTP_API(...)` maps a typed `FORGE_API(...)` contract onto native HTTP routes.
The binding is a composable artifact; `build()` does not mutate the router.

```cpp
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.http.binding;
import forge.api.http.proxy;
import forge.net.http.router;

struct read_chunk {
   std::string ref;
   std::uint32_t offset = 0;
   std::uint32_t limit = 0;
};

struct write_chunk {
   std::string ref;
   std::string bytes;
};

struct chunk {
   std::string bytes;
};

class cache : public forge::api::core::contract<
   cache,
   forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~cache() = default;

   virtual boost::asio::awaitable<chunk> read(read_chunk request) = 0;
   virtual boost::asio::awaitable<chunk> write(write_chunk request) = 0;
};

FORGE_API(
   cache,
   FORGE_API_CONTRACT("cache", 1, 0),
   FORGE_API_METHOD(read),
   FORGE_API_METHOD(write))

FORGE_HTTP_API(
   cache,
   FORGE_HTTP_GET(read, "/cache/chunks/:ref?offset={offset}&limit={limit}"),
   FORGE_HTTP_PUT(write, "/cache/chunks/:ref", created))

auto plan = forge::api::core::binding()
   .serve(app.apis())
   .export_api<cache>()
   .build();

auto binding = forge::api::http::binding()
   .use(plan)
   .bind<cache>()
   .build();

router.mount(binding);
```

HTTP stays HTTP: route/path/status semantics remain native. The transport does
not wrap typed calls in a message-frame body.

### FastAPI-Style DTO Parameters

For production HTTP endpoints, prefer one described request DTO. FastAPI-style
parameter categories live as DTO fields, not as a long positional method
signature. This keeps call sites readable, keeps validation paths named, and
keeps the HTTP-specific surface out of `forge_api_core`.

```cpp
struct write_payload {
   std::string bytes;
};

struct write_receipt {
   std::string id;
};

BOOST_DESCRIBE_STRUCT(write_payload, (), (bytes))
BOOST_DESCRIBE_STRUCT(write_receipt, (), (id))

struct update_item_request {
   std::string collection;
   std::string item;
   forge::api::http::query<std::uint32_t> ttl;
   forge::api::http::header<std::string> request_id;
   forge::api::http::body<write_payload> body;
};

BOOST_DESCRIBE_STRUCT(update_item_request, (), (collection, item, ttl, request_id, body))

class catalog_api : public forge::api::core::contract<catalog_api> {
 public:
   virtual ~catalog_api() = default;

   virtual boost::asio::awaitable<write_receipt>
   update_item(update_item_request request) = 0;
};

FORGE_API(
   catalog_api,
   FORGE_API_CONTRACT("catalog", 1, 0),
   FORGE_API_METHOD(update_item))

FORGE_HTTP_API(
   catalog_api,
   FORGE_HTTP_PUT(update_item, "/collections/:collection/items/:item?ttl={ttl}", created,
      FORGE_HTTP_HEADER(request_id, "X-Request-Id")))
```

Server binding fills `collection` and `item` from path placeholders, `ttl` from
the query string, `request_id` from `X-Request-Id`, and `body` from a JSON
request body. If a wire header or form name must differ from the DTO field name,
use the existing route options such as `FORGE_HTTP_HEADER(field, "Wire-Name")`
or `FORGE_HTTP_FORM(field, "wire-name")`. GET and HEAD routes that return a
non-200 success status declare it with `FORGE_HTTP_SUCCESS_STATUS(status)`.

The same typed client call builds the HTTP request:

```cpp
auto catalog = co_await forge::api::http::remote<catalog_api>(client);
auto receipt = co_await catalog->update_item({
   .collection = "cache",
   .item = "entry-1",
   .ttl = {.value = 3600, .present = true},
   .request_id = {.value = "trace-123", .present = true},
   .body = {.value = {.bytes = "payload"}, .present = true},
});
```

HTTP-only special request types include `query<T>`, `header<T>`, `cookie<T>`,
`body<T>`, `form<T>`, `form_field<T>`, `upload_file`, `body_bytes` and
`body_stream`, and they are supported as fields of a described request DTO.
The typed HTTP client supports JSON, raw bytes, streaming body and browser-style
multipart DTO fields without routing these wrappers through `forge.raw`.

HTTP positional methods remain available only as small routing sugar: scalar,
string, enum and optional arguments may bind to route path/query placeholders,
and at most one remaining described DTO argument may become the JSON body for a
body-capable route. HTTP wrappers such as `query<T>`, `header<T>`,
`body_stream`, `form<T>` and `upload_file` are not allowed in positional HTTP
signatures. Multi-argument APIs remain first-class for local, WebSocket, QUIC,
P2P, TCP and STCP bindings where there is no HTTP parameter model.

Special return types remain `file_response`, `streaming_response`,
`bytes_response` and `empty_response`. Background task injection is
intentionally out of scope; application runtime and plugin layers own
background work.

### Add Middleware

Low-level middleware can be installed directly on a router:

```cpp
router.use([](forge::net::http::route_context& ctx, forge::net::http::next_handler next)
   -> boost::asio::awaitable<forge::net::http::response> {
   if (ctx.request.find(forge::net::http::field::authorization) == ctx.request.end()) {
      co_return forge::net::http::make_text_response(
         ctx.request,
         forge::net::http::status::unauthorized,
         "missing authorization");
   }
   co_return co_await next();
});
```

Typed API bindings should contribute middleware through the binding artifact so
route plugins can be composed before the server starts:

```cpp
auto binding = forge::api::http::binding()
   .use(plan)
   .middleware(forge::net::http::middleware_descriptor{
      .id = "cache.authz",
      .phase = forge::net::http::middleware_phase::security,
      .order = 100,
      .path_prefix = "/cache",
      .handler = [](forge::net::http::route_context& ctx, forge::net::http::next_handler next)
         -> boost::asio::awaitable<forge::net::http::response> {
         authorize_cache_request(ctx.request);
         co_return co_await next();
      },
   })
   .bind<cache>()
   .build();

router.mount(binding);
```

Middleware contributions are sorted by `phase`, `order` and `id`. Duplicate
middleware ids and duplicate routes fail deterministically during
`router.mount(binding)`, before serving traffic.

### Start A Local Server

```cpp
import forge.asio.runtime;
import forge.net.http.server;

auto runtime = forge::asio::runtime{};
auto server = forge::net::http::server{
   runtime,
   {
      .bind_address = "127.0.0.1",
      .port = 8080,
      .max_request_body_bytes = 16 * 1024 * 1024,
      .max_header_bytes = 64 * 1024,
      .read_timeout = 30s,
      .idle_timeout = 120s,
   },
   std::move(router),
};

co_await server.async_start();
```

`read_timeout` bounds request socket reads. `idle_timeout` bounds socket
read/write operations and the gap between keep-alive requests; it does not bound
time spent awaiting a route handler or response body producer. Long-poll,
streaming and Server-Sent Events (SSE) owners may therefore wait longer than
`idle_timeout`, but they must apply their own method or application deadline
and remain cancellable when the peer disconnects. Applications must also
provide appropriate concurrency, backpressure and resource limits for those
waits.

### Use The Client

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.http.client;
import forge.net.http.types;

boost::asio::awaitable<void> check_ready(forge::net::http::client& client) {
   forge::net::http::response response = co_await client.async_get("/readyz");
   if (response.result() != forge::net::http::status::ok) {
      report_http_error(response.result(), response.body());
   }
}
```

### Use A Typed HTTP API

```cpp
#include <boost/asio/awaitable.hpp>

import forge.api.core.handle;
import forge.net.http.client;
import forge.api.http.proxy;

boost::asio::awaitable<void> read_chunk(forge::net::http::client& client) {
   forge::api::core::handle<cache> cache_api = co_await forge::api::http::remote<cache>(client);
   chunk value = co_await cache_api->read({
      .ref = "abc",
      .offset = 0,
      .limit = 64 * 1024,
   });
   consume(value);
}
```

### Send A JSON DTO

```cpp
#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>

import forge.net.http.client;
import forge.net.http.types;
import forge.codec.json;

struct action_request {
   bool dry_run = false;
};

BOOST_DESCRIBE_STRUCT(action_request, (), (dry_run))

boost::asio::awaitable<void> submit_action(forge::net::http::client& client) {
   auto body = forge::codec::json::write(action_request{.dry_run = true});
   if (!body.ok()) {
      report_diagnostics(body.diagnostics);
      co_return;
   }

   forge::net::http::response response = co_await client.async_post_json("/v1/actions", body.text);
   if (response.result() != forge::net::http::status::ok) {
      handle_http_error(response.result(), response.body());
   }
}
```

Raw JSON string literals are fine for tests and probes, but application APIs should
prefer described DTOs plus `forge_codec_json` so field names and diagnostics stay in one
place.

### WebSocket Upgrade Route

```cpp
import forge.net.websocket.connection;

router.websocket("/events", [](std::shared_ptr<forge::net::websocket::connection> ws) {
   // Own the connection lifecycle in the caller.
});
```

## Backpressure And Failure Model

Client requests are serialized through a per-connection queue. Retry behavior is
restricted to safe/idempotent cases covered by tests. Middleware can
short-circuit stream routes after headers and before body consumption.
Exceptions become typed HTTP responses at the route boundary, and stream body
limits/timeouts apply while chunks are read.

## Risks And Anti-Patterns

- Do not use HTTP routes as the authorization boundary. Middleware may call a
  consumer auth service, but application policy lives above `forge_net_http`.
- Do not retry mutating requests implicitly. The caller must decide whether an
  operation is idempotent and safe to replay.
- Do not log request bodies, headers or query strings before redaction. They may
  contain credentials or user data.
- Do not catch application exceptions in every route by hand. Prefer typed
  `forge_exceptions` categories and let API bindings project them to
  `forge::api::core::error_payload`.
- Do not force all typed APIs into a single generic RPC endpoint; use native HTTP route/status
  mapping where HTTP is the transport.
- Do not force file upload/download through `FORGE_API`; use stream routes and the
  file/upload helper layers.
- Do not hide server bind/TLS/lifecycle in `forge.api.http.binding`; the API builder owns
  route mapping, API middleware, status projection and error payloads only.
- Do not add HTTP API builder options unless they change runtime behavior and
  have tests.

## Typical Mistakes

- Do not parse full base URLs for every request target; use `base_url` for the
  origin and `target` for per-request paths.
- Do not put WebSocket server lifecycle in a separate `websocket::server`; v1
  upgrade starts from the HTTP server/router.
- Do not log headers or bodies containing credentials without redaction.
- Do not put authentication policy in `forge_net_http`; middleware can call a consumer
  auth service, but the policy owner is outside this library.

## Tests

`test_forge_http_websocket` covers base URL and target parsing, async router and
middleware behavior, stream request/response bodies, typed HTTP API mapping,
client/server roundtrip, reconnects and WebSocket upgrade.
