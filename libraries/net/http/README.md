# forge_net_http

`forge_net_http` is the HTTP substrate: URL parsing, FORGE-owned request/response
messages, cookie handling, bounded static assets, streaming body primitives,
routing, middleware, server and client/connection primitives. It uses Boost.Beast/URL internally but keeps
FORGE-owned public message, route and lifecycle semantics.

Application-level server lifecycle can be owned directly with `forge::net::http::server`
or composed through the official `forge.plugins.net.http.server` plugin. The library
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

- `forge.net.http.method` owns `method` and `is_idempotent()` in the public
  `forge_net_http_values` target (package component `net_http_values`). It has
  no transport, Asio, TLS or HTTP runtime dependency. It uses Raw metadata to
  preserve signed 32-bit scalar encoding across native/WASM/ABI. `forge.net.http.types`
  re-exports this module, so existing imports keep working.
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
Server observations additionally use `forge_log` as a private implementation dependency.

## Server Observability

The substrate uses the named FORGE logger `forge.net.http.server`. `http.listening`
is INFO only after socket listen and accept-loop publication; `http.drained` is
INFO only after accept, pending TLS handshakes and owned HTTP/WebSocket handlers
have joined. Repeated start/stop calls do not emit duplicate lifecycle successes.
Cleanup after an already stopped runtime emits `http.closed_after_runtime_stop`,
not a claim that live handlers have drained. Failures use numeric typed codes and
fixed error families (`forge`, `boost`, `std`, `unknown`), never exception text or
captured exception context. Expected cancellation, timeout and disconnect are
DEBUG; an HTTP 4xx status alone is not an operational error.

Request access logging is disabled by default. Both the option and logger DEBUG
level must be enabled:

```cpp
import forge.log.logger;
import forge.net.http.server;

auto route = forge::logger::get("forge.net.http.server");
route.set_log_level(forge::log_level::debug);
forge::logger::update("forge.net.http.server", route);

auto options = forge::net::http::server_config{};
options.access_log = true;
// Pass options to server(runtime, options, handler_or_router).
// Configure FORGE sinks through the application's ordinary logging setup.
```

Each received request has one DEBUG `http.request.finished` record. It contains
the numeric FORGE method enum (`unknown` for unsupported/incomplete methods),
status, monotonic `duration_us`, Beast-reported `received_bytes`/`sent_bytes`,
successfully written `response_chunks`, and `outcome=completed|aborted`.
Byte counts include HTTP framing/headers; speculative reads of pipelined input
are not attributed to the active request. `status=0` means no response status was
chosen, including a parser failure. Streaming completion is observed once after
the whole send and its send guard settle, not at headers or per chunk. A failed
producer, partial send, rejected send guard or observed disconnect is aborted;
a successfully sent error response is completed with its actual HTTP status.
WebSocket upgrade completion reports status 101 after successful acceptance,
not after the upgraded application's lifetime.
Beast reports a rejected upgrade only after its HTTP refusal has been written:
these records therefore retain the actual 400/426 status and completed outcome,
while transport failures stay aborted. Its accept operation exposes no byte
count, so upgrade records explicitly set `sent_bytes_known=false`; zero is not
presented as a measured handshake size.

No raw target/URI, route parameters, header, body, peer identity, certificate,
key material or configuration dump is logged. Route strings are deliberately
omitted, rather than attempting to sanitize a request-controlled path. Logging
is observational and noexcept: sink failures cannot change response, resource
ownership or cleanup outcomes, and sinks are never called under HTTP observation
mutexes. Configured console/file/JSONL delivery uses Forge Log's bounded
asynchronous adapter; OTLP owns its existing asynchronous queue. Direct custom
sinks must still be nonblocking or explicitly adapted. Record preparation is
synchronous; exception isolation is not a hard latency guarantee.

TLS handshake/session failures and TLS/header/body resource rejections use fixed, saturating
counters instead of one warning per connection or an unbounded peer-keyed map.
`http.admission.summary` is emitted at most once per five seconds on subsequent
activity, plus a final pending summary before `http.drained`. There is no timer
or background logging worker. TLS failures/context loss make the summary WARN;
expected disconnects and resource rejections alone stay DEBUG. Summary state is
copied/reset under its mutex and delivered only after releasing that mutex.

Focused capture/redaction/stream/error fixtures live in
[`tests/http/server_observability_tests.cpp`](../../../tests/http/server_observability_tests.cpp).
Existing TLS and shutdown-retry fixtures remain the compatibility gates for
snapshot lifetime, handshake bounds and real drain semantics.

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

For asynchronous credential acquisition, configure `async_header_provider`
instead of `header_provider`. Its signature is
`awaitable<vector<header_entry>>(const request&, request_preparation_context)`.
The context owns the exact transport `origin` (including the effective port)
and an absolute steady-clock `deadline`. The read-only request contains the
final method, target, existing headers and Host. Async preparation accepts
origin-form targets (or `OPTIONS *`), rejects duplicate/mismatched Host headers,
and supplies the endpoint authority when Host is absent. It does not let a
provider authorize one origin while sending an absolute target to another.
Both providers use the same header collision/framing/size validation; setting
both is a configuration error, not an implicit ordering or override policy.

Unary, streaming upload, streaming download and upload/download requests share
this preparation before enqueue/open/body consumption. Acquisition runs on the
caller's Asio executor, inherits cancellation, and consumes the same timeout
budget as transport and response-body reads. The deadline timer cancels the
provider; cancellation/deadline are checked again after completion, so a late
credential cannot initiate network I/O. Asio joins the canceled branch rather
than detaching it: providers must be nonblocking and settle promptly when
canceled. A provider that deliberately ignores cancellation can delay completion
until it settles, but cannot send after the deadline/cancellation. The request
reference must not be retained beyond provider completion, and concurrent calls
require a concurrency-safe provider.

`request_options::deadline` optionally supplies a caller-owned absolute
steady-clock deadline. Preparation/transport use the earlier of it and
`timeout`; the effective absolute deadline is preserved through the connection
queue, retries and streamed response body, not restarted after acquisition.

Auth/token-provider composition belongs to the consumer, once per client, not
to each API method. HTTP client preparation imports neither Auth nor API; the
existing server router still uses API Core's canonical error payload. The new
preparation and lifetime hooks add no Auth/API dependency to the transport.
The client does not follow redirects, refresh on 401, or replay on timeout. Existing
explicit idempotent transport-reset retries are unchanged; mutations with an
uncertain outcome need application-owned operation-ID/status recovery. Never
log credentials, provider output or secret-bearing exception diagnostics.

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

For a server-owned session, use `websocket_guarded` and return
`websocket_upgrade_result::session`, not a callback that spawns detached work:

```cpp
import forge.net.http.router;
import forge.net.websocket.connection;

router.websocket_guarded("/events", [](forge::net::http::route_context&)
   -> boost::asio::awaitable<forge::net::http::websocket_upgrade_result> {
   co_return forge::net::http::websocket_upgrade_result{
      .session = [](std::shared_ptr<forge::net::websocket::connection> socket)
         -> boost::asio::awaitable<void> {
         co_await socket->send("ready");
      },
   };
});
```

The admission coroutine runs before `101` and inherits disconnect/shutdown
cancellation. Rejection cannot fall through to an ordinary route. An accepted
result selects exactly one of `handler` and `session`; the former preserves the
caller-owned callback contract. For `session`, the server starts WebSocket reads,
awaits the handler, closes the connection and only then completes the HTTP session.
Shutdown cancels and joins that same coroutine, including its cooperative cleanup.
The session must join any application children before returning, even by exception.
Keep registries, producers and other nonowning dependencies alive until
`server.async_stop()` completes, before stopping the shared runtime.
WebSocket transport `connection::cancel()` aborts pending native I/O without a
close handshake; `connection::close()` remains graceful for normal completion
and joins an active write's completion after cancellation. Session owners must
abort before draining a blocked writer: queueing a graceful close behind that
writer cannot stop a connected peer that never reads. The transport does not
decide when application policy requires this abort.

### Own The Complete Response Send

`stream_response::send_guard` is a neutral awaitable lifetime hook. It receives
the complete send operation and an `abort` callback that cancels and closes the
HTTP socket on its executor. The operation includes response headers, body-source
waits, every socket write and the final chunk/trailer, including buffered responses
returned by a stream route. The server awaits the guard exactly once; a body source
alone cannot govern a write stalled by a slow client.
Middleware response projection preserves this hook even when it replaces a stream
body with buffered output; it does not silently detach the response's owner.

The owner supplies policy through this hook and must await/join the operation on
success, failure and cancellation. If policy ends, call `abort` before waiting for
cleanup so pending socket I/O cannot continue sending. Do not retain the operation
or abort callback after guard completion, or start a detached watchdog, per-chunk
timer framework or independent runtime. API bindings use their existing canonical
invocation guard here; the transport owns no subject, expiry or revocation policy
and these hook declarations import neither Auth nor API.

After response headers, abort the send rather than sending a second HTTP status.
Bytes already accepted by the kernel or delivered to the peer cannot be retracted;
aborting prevents further application writes and leaves a partial response, never
a successful end-of-body after a lifetime failure.

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
- Do not detach a server-owned upgraded session or protect only body production
  while leaving its socket writes outside the lifetime guard.
- Do not log headers or bodies containing credentials without redaction.
- Do not put authentication policy in `forge_net_http`; middleware can call a consumer
  auth service, but the policy owner is outside this library.

## Tests

`test_forge_http_websocket` covers base URL and target parsing, async router and
middleware behavior, stream request/response bodies, typed HTTP API mapping,
client/server roundtrip, reconnects and WebSocket upgrade.

`test_forge_api_trusted_subject` exercises the neutral lifecycle hooks through an
API-layer fixture: live upgraded-session shutdown, pre-101 admission cancellation,
slow socket writes interrupted by expiry/revocation, and active-handler cleanup
joined before session completion. It also leaves a WebSocket peer connected but
nonreading to check that revocation and shutdown abort a stalled native write
and join the active handler before completing. These are loopback regressions, not evidence
of a production identity provider or external verifier backend.

`test_forge_http_client_preparation` covers the generic async provider against
real loopback HTTP: all four request shapes, typed proxy composition and stream
preparation failures, shared header validation, exact origin metadata,
nonblocking acquisition on one worker, cooperative timeout, late cancellation
and late timeout, acquisition/transport/body deadline accounting, and no
redirect forwarding or 401/timeout replay. These transport fixtures do not
prove live AppAuth/oidc-agent login, refresh or workload token exchange.
