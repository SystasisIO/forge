# forge_api_http

For native records with explicit `schema::rules<T>`, OpenAPI uses
`schema::export_constraints<T>()` and the same `schema::json_schema()` exporter
as standalone artifacts. Canonical schema names, required fields, exact integer
ranges, sizes, constants/enums and decimal-string bounds are retained in both
record schemas and request field metadata. Unrepresentable rules fail with
`schema::exceptions::unsupported_export`; they are never silently omitted.
The supported bounded subset is documented by `libraries/schema/README.md`.

`FORGE_HTTP_RESPONSE_BODY_LIMIT(bytes)` (or route_builder::response_body_limit)
sets the finite client response payload bound for that route. Generated DTO and
positional clients apply it through the existing transport request_options on
buffered and streamed responses. Without this metadata the transport default
remains 8 MiB; zero permits only an empty body. This is local client transport
policy, never a request DTO or HTTP header, and does not change timeout/retry
policy or disable server-side limits.

## Server Admission

Call `binding_builder::invocation(provider)` before registering/binding routes.
The async provider receives the selected route context and canonical method
descriptor, before domain dispatch, stream-open or streaming request-body reads.
It returns `forge::api::core::invocation_context`. Choose resource policy from
trusted route configuration; do not promote headers/JSON into verified subjects.

With a nonempty provider, unary methods also use the existing stream route to run
admission before consuming a buffered request body. After admission, ordinary DTOs
and positional arguments retain their buffered decoding and native request body.
An early denial sends the final response and closes an unread request; it does not
send `100 Continue`. The provider runs once. Publications without a provider keep
their existing buffered/stream route selection.

For manual in-process dispatch, select `router::handle_stream` when
`router::can_handle_stream` reports a stream route, as the HTTP server does.
Supply the request body through its `body_reader`. Opting into a provider makes
an ordinary unary route a stream route; `router::handle` dispatches buffered
routes only.

The binding composes provider values with the verified TLS caller and rejects any
duplicate type. Required server-supplied values are still reset/applied by the
existing descriptor mechanism. Subject validity bounds admission and stream
lifetime, including buffered output after the method finishes. A persistent
response-body recheck schedule prevents fast chunks from postponing revocation.
After response headers, denial cancels the body/transport instead of inventing a
new HTTP status. Provider policy owns credential ambiguity, Origin/CSRF and product
permissions. Capture only safe owned references in recheck hooks, not the HTTP
request or bearer; provider completion checks caller cancellation before dispatch.

```cpp
auto routes = forge::api::http::binding()
   .use(plan)
   .invocation(admit_selected_route)
   .bind<resource_api>()
   .build();
```

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

### JSON Schema Fidelity

Generated integer schemas carry exact C++ minimum/maximum bounds, including
uint64 maximum `18446744073709551615`, without conversion through floating point.
Existing `int32`, `uint32`, `int64` and `uint64` format labels are unchanged; bounds,
not recognition of a vendor format label, enforce the narrower integer domain.
An indexed `std::variant` is `oneOf` complete two-item arrays: each branch binds
its constant index to the corresponding payload schema. Overlapping payload
shapes remain distinct because their indexes differ. The JSON wire stays
`[index, payload]`.

Typed JSON requests, success responses and error envelopes use the existing exact JSON decoder.
Integers must have the correct type and fit their destination; missing required
members, unknown members and malformed indexed variants are rejected before the
service receives them. Explicit schema rules remain responsible for their own
defaults and constraints. Raw, XML and the standalone JSON codec's defaults are
unchanged. Custom scalar adapters retain their own canonical representation.

This is an explicitly approved input-compatibility break. Clients must send
numeric JSON values rather than numeric strings, all required members, and
exactly `[index, payload]` for indexed variants. Neither missing values nor
overflow are repaired by defaulting, truncation or wrapping. Optional members
still accept omission or null, and absent explicit `body<T>` wrappers retain
`present == false`. Path/query/header/cookie text conversion is unchanged.
Malformed typed JSON yields the binding's existing validation error response.
An absent required JSON body is rejected; bodyless requests with all required
fields bound from the route remain valid. A malformed error envelope maps to
the generic HTTP API failure without copying its raw body into the exception.

The [Python qualification](../../../tests/api/openapi_python/README.md) uses an
actual pinned generator and unmodified Pydantic v2 models. Its consumer validates
the original JSON against the exported schema before calling the generated model;
this rejects boolean variant indexes even when stock Pydantic accepts them as
integer literals. The report separates raw-model behavior from this validated
consumer, schema and HTTP codec verdicts. JSON Schema mathematical integer
equality still differs from exact token handling. This is qualified coverage,
not universal accepted-input language equality or support for arbitrary custom
schema rules/adapters. The `x-forge-json-profile` annotation documents this
boundary; it is not an extra validator.
Server-supplied fixed arguments remain excluded from client parameters/bodies;
ordinary nested fields are not recursively removed.

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

### Compose Asynchronous Credentials Once

Every typed unary, body-stream and live API stream call uses the configured
`forge::net::http::client` preparation. Do not wrap each API method, block inside
the synchronous `header_provider`, or add a second owner of `Authorization`.
Composition may import an acquisition backend; the HTTP substrate does not.
For example, with an already configured `token_provider`, trusted issuer/resource
profile and HTTPS endpoint:

```cpp
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

import forge.auth.oauth2.token_provider;
import forge.net.http.client;
import forge.api.http.proxy;

auto options = forge::net::http::client_options{
   .async_header_provider =
      [tokens, token_profile, expected_issuer, allowed_origin = endpoint.origin()]
      (const forge::net::http::request&, forge::net::http::request_preparation_context context)
         -> boost::asio::awaitable<std::vector<forge::net::http::header_entry>> {
         if (context.origin != allowed_origin) {
            throw forge::auth::oauth2::exceptions::invalid_request{"Unexpected resource origin"};
         }
         auto token = co_await tokens->async_access_token(token_profile);
         const auto& metadata = token.metadata();
         if (metadata.issuer != expected_issuer || !metadata.expires_at ||
             *metadata.expires_at <= std::chrono::system_clock::now()) {
            throw forge::auth::oauth2::exceptions::token_unavailable{"Acquired token does not match the profile"};
         }
         co_return std::vector<forge::net::http::header_entry>{
            {"Authorization", "Bearer " + std::string{token.secret().view()}}};
      },
};
auto client = forge::net::http::client{runtime, endpoint, std::move(options)};
auto remote = co_await forge::api::http::remote<catalog_api>(client);
```

`tokens` is a configured acquisition provider, not a server token verifier;
`token_profile` is a fixed `forge::auth::oauth2::token_request` with the intended
scopes/audience/minimum validity. AppAuth requires it to match its configured
profile. Providers must support cancellation and the selected resource profile;
an audience parameter alone is not evidence that an IdP issues that audience.
The request/context are also available for narrower resource selection outside
the net layer. Never pass a projected workload assertion as an access token.

Acquisition, send and stream reads consume one HTTP request deadline. Provider
failures reject before network I/O; late cancellation cannot open an HTTP
request. The client does not follow redirects or refresh-and-replay after 401
or timeout. Use a new explicit call with an updated credential, and recover
uncertain mutations through an application operation ID/status query.

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
- A specialization may explicitly set `static constexpr bool http_visible = false`
  to hide that service value when it is a reflected request member. Without this
  opt-in, nested DTO bodies and OpenAPI retain their previous representation.
  Hidden members are omitted by the HTTP client and request OpenAPI, do not
  require a request body and cannot be mapped to path/query/header/form fields.
  A client-supplied hidden JSON key is rejected; missing/unknown/duplicate ordinary
  payload fields still pass through the existing exact JSON codec. Empty defaults
  are materialized solely for decoding before core reset/apply supplies trusted
  values. The marker does not change Raw, P2P, response encoding or core traversal.
  This profile follows direct reflected members, including inherited members and
  described records nested in them. As in core dispatch, it does not implicitly
  traverse optionals, ranges, variants or pointers: their contents remain ordinary
  payload values and do not acquire trusted authority from this marker. A hidden
  value inside an HTTP parameter wrapper such as `body<request>` is unsupported
  and rejected when building the route. Put the service member beside an ordinary
  `body<business_payload>`, query, form or stream member instead. Hidden members
  and their containing described members require single-segment schema names
  and aliases; dotted names are rejected with typed `incompatible_version` before
  mount, client encoding or OpenAPI generation. Ordinary dotted payload schemas
  retain the codec's existing nested-object behavior.
  The whole-request body form of this opt-in profile supports JSON; XML is
  rejected with typed `incompatible_version` during route validation. Existing
  XML DTOs and explicit XML `body<T>` wrappers without hidden fields in `T`
  retain their behavior. Query, multipart and streaming request members continue
  to use their existing bindings. An ordinary business payload key with the same
  spelling as a service member is unaffected.
- JSON/XML DTO codecs apply only to typed DTO bodies. `file_response`,
  `streaming_response`, `stream_response`, `bytes_response`, `empty_response`,
  `body_stream`, `body_bytes` and multipart/form-data bypass DTO codecs.
- Error bodies use the route error codec and the shared Forge API error payload
  shape by default. `binding().errors(renderer)` supplies a publication default;
  `route_options::error_renderer` can override it for an explicit route.
- Do not log request bodies, headers or query strings before redaction.
- Keep protocol-specific error names, signing, authorization and storage policy
  outside this library.

## External Error Representations

`forge.api.http.error_renderer` is an optional synchronous presentation boundary.
The callback receives `error_context`: the original request, proposed HTTP status,
sanitized core error payload and, for a local catch, a borrowed exception pointer.
It returns an owned native HTTP response or `std::nullopt` to retain the standard
representation. A renderer failure also retains the standard representation.

The context and its exception pointer must not escape the callback. A callback
may inspect explicitly supported typed exception data; it must not expose
`what()`, private diagnostic fields or undeclared exceptions. Already projected
remote errors have no local exception pointer. Unexpected non-Forge failures also
reach an installed renderer as a safe internal error with no exception pointer;
without a renderer their existing exception propagation remains unchanged. Status and body customization
also covers parsing and content negotiation failures before method invocation.
Endpoint headers and the route cache policy are applied to the final response.
The callback owns the returned response's HTTP version, keep-alive and payload
framing; `forge::net::http::make_text_response(context.request, ...)` provides
these mechanics for an ordinary buffered response.
This hook does not intercept errors emitted after an API stream has started;
those still use the stream's existing terminal error frames.

Install the renderer before adding routes with `.bind<Interface>()` or `.post()`.
The default remains unchanged for all existing API consumers. Protocol-specific
DTOs and renderers belong to the application, not Forge.

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

- `test_forge_http_error_renderer`
- `test_forge_api_core`
- `test_forge_http_websocket`
- `test_forge_package_api_http_component`
