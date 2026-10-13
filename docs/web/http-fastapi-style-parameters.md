# HTTP FastAPI-Style Parameters

This design note records the implemented direction for DTO-first
`FORGE_HTTP_API(...)` bindings, bounded HTTP positional methods and the
boundaries to keep clean.

The goal is FastAPI-style endpoint ergonomics for C++ without turning HTTP into
a generic frame RPC transport:

- `FORGE_API(...)` remains the typed contract metadata source.
- `FORGE_HTTP_API(...)` declares only HTTP presentation: method, path, success
  status and response mode.
- Request parameters are classified from described DTO fields first.
- HTTP positional arguments are limited to path/query routing sugar plus at most
  one described JSON body DTO.
- Boost.Beast request/parser/serializer mechanics stay private to `forge_net_http`.
- `forge_api_core` stays HTTP-free.

Donor behavior:

- FastAPI recognizes `Query`, `Path`, `Body`, `Cookie`, `Header`, `Form` and
  `File` as request parameter helpers.
- FastAPI also supports request/response context objects in path operation
  functions.
- FastAPI can inspect Python function parameter names at runtime. C++ cannot,
  so FORGE records positional names in `FORGE_API_METHOD(method, arg...)` while
  keeping HTTP parameter categories in wrapper types.

References:

- [FastAPI request parameters](https://fastapi.tiangolo.com/reference/parameters/)
- [FastAPI request body](https://fastapi.tiangolo.com/tutorial/body/)
- [FastAPI query parameters](https://fastapi.tiangolo.com/tutorial/query-params/)
- [FastAPI form data](https://fastapi.tiangolo.com/tutorial/request-forms/)
- [FastAPI response context](https://fastapi.tiangolo.com/advanced/response-headers/)

## Target Shape

Consumers should normally write one C++ API method that accepts one described
request DTO. FastAPI-style parameter categories are represented as fields:

```cpp
struct object_request {
   std::string bucket;
   std::string key;
   forge::api::http::query<std::uint32_t> limit;
   forge::api::http::header<std::string> request_id;
};

BOOST_DESCRIBE_STRUCT(object_request, (), (bucket, key, limit, request_id))

class object_api {
 public:
   virtual boost::asio::awaitable<object_meta>
   get_object(object_request request) = 0;
};

FORGE_API(
   object_api,
   FORGE_API_CONTRACT("object", 1, 0),
   FORGE_API_METHOD(get_object))
```

The HTTP presentation stays compact:

```cpp
FORGE_HTTP_API(
   object_api,
   FORGE_HTTP_GET(get_object, "/:bucket/:key?limit={limit}",
      FORGE_HTTP_HEADER(request_id, "X-Request-Id")))
```

There is no per-argument mapping macro in the normal path.
`FORGE_HTTP_API(...)` declares the HTTP route and presentation. The binding layer
derives parameter sources from DTO field names and wrapper types:

- `bucket` and `key` are filled from `/:bucket/:key`.
- `query<std::uint32_t> limit` is filled from query parameter `limit`.
- `header<std::string> request_id` reads `X-Request-Id` through the explicit
  route alias. Without an alias, the default wire name is `request-id`.
- The generated client proxy builds the corresponding Boost.Beast request
  internally.

## Upload And Body Example

Streaming upload should be expressed as a DTO field, not as a long positional
signature:

```cpp
struct put_object_request {
   bucket_name bucket;
   object_key key;
   forge::api::http::header<std::string> type;
   forge::api::http::body_stream body;
};

BOOST_DESCRIBE_STRUCT(put_object_request, (), (bucket, key, type, body))

class object_api {
 public:
   virtual boost::asio::awaitable<put_object_response>
   put_object(put_object_request request) = 0;
};

FORGE_API(
   object_api,
   FORGE_API_CONTRACT("object", 1, 0),
   FORGE_API_METHOD(put_object))

FORGE_HTTP_API(
   object_api,
   FORGE_HTTP_PUT(put_object, "/:bucket/:key", created,
      FORGE_HTTP_HEADER(type, "Content-Type"),
      FORGE_HTTP_BODY_STREAM(body)))
```

Binding:

- `bucket` and `key` consume route path placeholders.
- `header<std::string>` consumes an HTTP header by field name, or by
  explicit `FORGE_HTTP_HEADER(...)` alias.
- `body_stream` remains the streaming request-body type for APIs that should
  not buffer the entity body.

## Special Types

The first-class HTTP parameter vocabulary is small and explicit. These types
are supported as described request DTO fields:

| Type | Meaning |
| --- | --- |
| `forge::api::http::query<T>` | Query parameter value decoded by field name or route query alias. |
| `forge::api::http::header<T>` | Header value decoded by explicit `FORGE_HTTP_HEADER(...)` alias or default field-name mapping `_ -> -`. |
| `forge::api::http::cookie<T>` | Cookie value decoded by field name. |
| `forge::api::http::body<T>` | Explicit JSON body DTO field. |
| `forge::api::http::body_bytes` | Bounded raw body bytes. |
| `forge::api::http::body_stream` | Streaming request body. |
| `forge::api::http::form<T>` | Form field value decoded by field name or form alias. |
| `forge::api::http::form_field<T>` | Server-side named form field. |
| `forge::api::http::upload_file` | Server-side multipart file part with safe filename helpers and bounded spool behavior. |

The typed HTTP client supports DTO fields for ordinary JSON, `query<T>`,
`header<T>`, `cookie<T>`, `body<T>`, `body_bytes`, `body_stream`, `form<T>`,
`form_field<T>` and `upload_file` without falling back to `forge.raw`.
XML body and error codecs are planned in
[Forge XML And HTTP API Codec Plan](../iterations/forge-xml-http-api-codec-v1.md).
JSON remains the default; XML is route-selected presentation, not a separate
manual router path.

FastAPI-style background task injection is intentionally not part of this surface.
Background execution policy belongs to the application runtime, scheduler,
plugin lifecycle or product job system, not to HTTP parameter binding.

Response special types remain return values, not request parameters:

- `forge::net::http::file_response`;
- `forge::net::http::streaming_response`;
- `forge::api::http::bytes_response`;
- `forge::api::http::empty_response`.

## Binding Rules

DTO binding is deterministic and fail closed:

1. Match path placeholders against described DTO field names.
2. Decode `query<T>` by route query alias or field name.
3. Decode `header<T>` by explicit HTTP header alias or default field-name
   mapping `_ -> -`.
4. Decode `cookie<T>`, `form<T>` and `form_field<T>` by field name unless an
   explicit alias is provided.
5. Bind `body<T>`, `body_bytes`, `body_stream`, `form<T>`, `form_field<T>` and
   `upload_file` by field type.
6. If no explicit body field or HTTP parameter wrapper exists, preserve legacy
   whole-request body DTO behavior using the route-selected codec. JSON remains
   the default. A DTO with `query`, `header` or `cookie` wrappers uses an explicit
   `body<T>` field when it also needs a structured body.
7. Run final `forge_schema` validation after all HTTP sources are assembled.
8. Reject ambiguous mappings at compile time when the type information is
   enough, otherwise at mount time before the server starts.

There must be no silent guessing when two fields could consume the same path,
query, header, cookie, form or body source.

## Bounded HTTP Positional Parameters

FORGE still supports positional API methods for local and frame transports. For
HTTP, positional methods are intentionally bounded:

```cpp
boost::asio::awaitable<object_meta>
get_object(std::string bucket, std::string key, std::optional<std::uint32_t> limit);

FORGE_API(
   object_api,
   FORGE_API_CONTRACT("object", 1, 0),
   FORGE_API_METHOD(get_object, bucket, key, limit))

FORGE_HTTP_API(
   object_api,
   FORGE_HTTP_GET(get_object, "/:bucket/:key?limit={limit}"))
```

Rules:

- ordinary scalar, string, enum and optional positional arguments may bind only
  to route path placeholders or route query placeholders;
- exactly one remaining described DTO argument may become the route-selected
  body codec payload for `POST`, `PUT`, `PATCH` and body-capable `DELETE`;
- remaining scalar/string/enum/optional arguments are errors if not consumed by
  path/query;
- `forge::api::http::query<T>`, `header<T>`, `cookie<T>`, `body<T>`, `form<T>`,
  `form_field<T>`, `upload_file`, `body_bytes` and `body_stream` are DTO-only
  for HTTP and are rejected in positional HTTP signatures.

The preferred production shape remains a described DTO for HTTP endpoints.
Positional HTTP exists for simple path/query routes and for compatibility with
the broader multi-argument `FORGE_API` model.

## Error Model

HTTP parameter binding errors should project to stable HTTP responses:

- invalid target/path syntax -> `400 Bad Request`;
- missing or invalid path/query/header/cookie/form fields -> `422
  Unprocessable Entity`;
- unsupported body media type -> `415 Unsupported Media Type`;
- body or multipart limits -> `413 Payload Too Large`;
- ambiguous or invalid route binding -> mount-time typed error.

Diagnostics should use source-aware paths such as:

- `path.bucket`;
- `query.version_id`;
- `header.Authorization`;
- `cookie.session`;
- `body.metadata.content_type`;
- `form.avatar`;

Diagnostics must not include secrets, bearer tokens, cookies or raw private
values.

## Boundaries

- This is an HTTP presentation layer over `FORGE_API(...)`, not a new product API
  framework. S3-style gateways must use this API binding model rather than
  manual router bypass for normal endpoints.
- FORGE does not add S3, SigV4, bucket, object-policy, billing or tenant-auth
  semantics.
- `forge_api_core` must not import `forge_net_http`.
- HTTP special parameter types make an API method HTTP-bound and should appear
  as fields of HTTP request DTOs. Transport-neutral APIs should keep ordinary
  request/response DTOs or use positional arguments without HTTP wrappers.
- Boost.Beast remains a private mechanics dependency.
- Manual per-argument macros are not part of the normal path. Existing
  `FORGE_HTTP_HEADER(...)`, `FORGE_HTTP_FORM(...)` and response mode options only
  express wire aliases or response presentation.
- Background task injection is out of scope for `forge_net_http`; HTTP endpoints may
  submit work through explicit application/plugin APIs instead of hidden
  framework-managed post-response jobs.
