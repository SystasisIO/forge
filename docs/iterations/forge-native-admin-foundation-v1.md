# Forge Native Admin Foundation v1

## Status

This document defines the Forge work needed by native C++ backends that serve a
browser administration application and call typed downstream services. It is an
implementation plan, not a shipped API contract.

The first downstream consumer is the optional Spine Admin product. Public Forge
types, targets, modules and configuration remain product-neutral.

## Production Status

The production-targeted Forge foundation described here is implemented in this
branch, including shared TLS context rotation, the HTTP server, browser
authentication and bounded asset serving. This does not by itself claim that a
downstream Spine Admin deployment is production-ready.

Downstream production acceptance remains future work. Spine Admin must validate
its production configuration and frontend bundle in both supported deployment
modes:

- self-contained mode, where the native backend serves the installed frontend
  bundle and terminates HTTPS itself;
- reverse-proxy mode, where nginx, Caddy or an ingress serves the same bundle
  and proxies only the typed backend API.

Static files are not an authorization boundary in either mode. Every protected
API request is authenticated and authorized by the native backend itself.

## Approved HTTP Source Migration

The native-admin foundation includes an approved pre-stabilization clean break
in the Stable `forge.net.http.types::response` source surface. The removed
member is not restored through an alias or forwarding overload. Migrate each
call mechanically:

```cpp
// Before
response.set_cookie(name, value);

// After
forge::net::http::append_set_cookie(
   response,
   forge::net::http::set_cookie{.name = name, .value = value});
```

The replacement is owned by `forge.net.http.cookie` and preserves repeated
`Set-Cookie` fields through `response::insert`. Before the branch is promoted
to a release, its release notes must name `response::set_cookie` as the affected
Stable source surface and repeat this mechanical migration. No compatibility
alias is required for this maintainer-approved pre-stabilization break.

The bounded file-I/O correction also makes ownership explicit on the low-level
Stable file surfaces. Migrate `file_response::from_path(path, options)` and
`static_file_root(root, options)` by inserting the caller-owned
`forge::asio::compute::pool::get_executor()` before `options`. Migrate direct
`router::mount_assets(mount)` calls to
`router::mount_assets(mount, file_reads.get_executor())`. The HTTP Server plugin
API remains `mount_assets(asset_mount)` because the plugin injects its
application `context.compute()` executor. Release notes must list these three
low-level signatures alongside the cookie migration; no executor compatibility
overloads are retained.

## Goal

Provide reusable browser authentication and HTTP publication mechanics without
creating a frontend framework or another remote-service client layer:

```text
browser
   |
   | same-origin HTTPS, owner session and CSRF protection
   v
native product backend
   |-- product workflow FORGE_HTTP_API
   |-- prebuilt TypeScript assets
   |-- forge.auth pairing and session state machines
   `-- existing typed Forge clients
```

The native backend remains the browser authority boundary. The browser does not
receive downstream service credentials, private signing keys or an unrestricted
proxy to every downstream API.

## Dependency Decision

No new external C++ dependency is required for the owner-only v1 design.

Forge already provides the required foundations:

| Need | Existing owner |
| --- | --- |
| Async runtime and cancellation | `forge_app`, `forge_asio`, Boost.Asio |
| HTTP client, server, router and files | `forge_net_http`, Boost.Beast, Boost.URL |
| TLS contexts and PKI validation | OpenSSL, Forge Crypto PKI and existing STCP donor mechanics |
| Typed API and OpenAPI | `forge_api_core`, `forge_api_http` |
| HTTP lifecycle and middleware | `forge_plugins_net_http_server` |
| Cryptographic random | `forge_crypto_core`, backed by OpenSSL `RAND_bytes` |
| Token digests | `forge_crypto_digest` |
| Base64URL | `forge_codec_base64` |
| JSON | `forge_codec_json`, backed privately by Glaze |
| Typed failures | `forge_exceptions` |
| Config and secret delivery | Forge Config and Crypto Secrets |
| Product persistence | Forge DB Object and MDBX through the DB Store plugin |
| Structured operational events | `forge_log` and optional OTLP adapters |

Boost supplies transport and application mechanics; it is not used as a
substitute for cryptographic random, constant-time comparison or durable
transactions. Those operations stay behind existing Forge crypto and DB
boundaries.

OIDC, JOSE/JWT, JWKS, enterprise identity, password authentication and RBAC are
outside v1. A future multi-user block must select a mature implementation after
a separate donor and security review. JWT or OIDC must not be implemented
manually with Boost.JSON and OpenSSL primitives.

### Mandatory server-side TLS gap

Current `forge_net_http` TLS support is asymmetric. The HTTP client creates a
verified `boost::beast::ssl_stream<boost::beast::tcp_stream>` for HTTPS, while
the HTTP server accepts a plain TCP socket and creates only
`boost::beast::tcp_stream`. `forge::net::http::server_config` and the HTTP
Server plugin consequently expose no certificate, private key, trust or TLS
handshake policy.

Linking OpenSSL into `forge_net_http` does not make the inbound listener a TLS
server. Closing this gap is mandatory in this iteration. Self-contained
production mode must provide native HTTPS without nginx, Caddy or an ingress.

The implementation uses Boost.Beast TLS server streams. It must not add a new
HTTP server backend and must not shell out to an external TLS process.

## Library Ownership

`forge.auth` becomes an empty family root. It has no aggregate target or module.
The implementation introduces three independent leaf libraries.

### `forge_auth_pairing`

```text
target:     forge_auth_pairing
namespace:  forge::auth::pairing
modules:    forge.auth.pairing.*
```

Owns product-neutral first-owner and device pairing records and transitions:

- high-entropy, short-lived bootstrap tokens;
- digest-only persisted token representation;
- bounded pending requests and expiry;
- explicit approval and rejection;
- superseding a request when identity or requested scopes change;
- one-time token consumption before reporting success;
- scope baselines, down-scope rotation and no implicit escalation;
- credential rotation and revocation;
- typed replay, expiry, capacity and state-transition failures.

It does not own HTTP, cookies, UI, CLI, a database driver, product roles or
automatic approval policy.

### `forge_auth_session`

```text
target:     forge_auth_session
namespace:  forge::auth::session
modules:    forge.auth.session.*
```

Owns opaque browser-session records and transitions:

- random session and CSRF material;
- digest-only persisted secrets;
- absolute and idle expiry;
- renewal, rotation, revocation and logout;
- session fixation resistance when pairing becomes an owner session;
- device identity and approved-scope binding;
- bounded validation and typed failure categories.

It does not own browser headers, persistence, product permissions or chain
signing policy. A browser session proves access to product workflows; it is not
a transaction signature or a downstream service credential.

### `forge_auth_http`

```text
target:     forge_auth_http
namespace:  forge::auth::http
modules:    forge.auth.http.*
```

Adapts pairing/session decisions to neutral HTTP security mechanics:

- session and CSRF cookie policy built on `forge.net.http.cookie`;
- enforcement of `Secure`, `HttpOnly`, `SameSite`, `Path`, expiry and
  `__Host-` invariants for authentication cookies;
- exact Origin allow-list checks;
- CSRF cookie/header verification for state-changing methods;
- session extraction and response cookie construction;
- browser security-header policy;
- redacted typed authentication failures;
- hooks for product-owned rate limiting and audit events.

The library depends on `forge_auth_session` and `forge_net_http`. Generic
Cookie and Set-Cookie parsing and formatting remain owned exclusively by
`forge.net.http.cookie`; `forge_auth_http` neither duplicates nor wraps a
second parser. It does not
depend on `forge_plugins_net_http_server`: libraries must not import product or
plugin runtime APIs. A product constructs a server-plugin middleware descriptor
around these neutral functions.

## Existing Forge Changes

### `forge_net_tls`

Create a focused TLS leaf shared by secure TCP, the existing HTTPS client and
HTTP server mechanics:

```text
target:     forge_net_tls
namespace:  forge::net::tls
modules:    forge.net.tls.options
            forge.net.tls.context
            forge.net.tls.exceptions
```

It owns reusable OpenSSL and Boost.Asio TLS mechanics:

- bounded certificate chain, private key and trust-anchor loading;
- certificate/private-key consistency validation;
- TLS protocol floor and secure context defaults;
- server and client verification modes;
- client-certificate validation for mTLS, always including certificate-chain
  verification against configured trust anchors;
- SNI and ALPN policy values;
- peer certificate extraction and typed verification failures;
- immutable context snapshots suitable for atomic credential rotation.

The existing STCP implementation is the donor for these mechanics. Shared
context construction is extracted rather than copied, and both STCP and the
existing HTTPS client migrate to the same builder. STCP public behavior and
P2P contracts do not change as part of the extraction.

`forge_net_tls` does not own HTTP, TCP listening, secret ids, config documents,
certificate issuance, ACME or product trust policy.

### `forge_crypto_core`

Add one public constant-time byte comparison primitive backed by the existing
OpenSSL boundary. Migrate the private implementation in HTTP Bearer
authentication to the shared primitive.

Authentication code compares fixed-size token digests, not clear variable-size
tokens. Length and malformed-input checks happen before the digest comparison.

No new random implementation is needed. Pairing and session tokens use the
existing `forge::crypto::core::random_bytes` or `random_array` APIs.

### `forge_net_http`

Add focused modules inside the existing target:

```text
forge.net.http.cookie
forge.net.http.assets
```

`cookie` owns RFC-constrained parsing and serialization mechanics. Auth policy
such as which cookie names or scopes are allowed remains in `forge_auth_http`.

`assets` maps an installed, prebuilt frontend bundle to read-only HTTP
responses. It must provide:

- GET and HEAD only;
- canonical path resolution below one configured root;
- rejection of `..`, encoded traversal, NUL, separator confusion and symlink
  escape;
- descriptor-relative opening without following symlinks, followed by
  validation that the already opened object is a regular file;
- no directory listing;
- bounded files and headers;
- MIME selection from an allow-listed table;
- ETag and conditional response support;
- immutable caching for fingerprinted assets;
- no-cache policy for `index.html`;
- an explicit optional single-page-application fallback.

It does not build TypeScript, upload files, watch source directories, execute
Node.js or become a general web framework.

The existing HTTP server additionally gains native TLS sessions based on:

```cpp
boost::beast::ssl_stream<boost::beast::tcp_stream>
```

The listener selects plain or TLS mode once from immutable server options. In
TLS mode every accepted socket completes a bounded server handshake before any
HTTP bytes are parsed. A failed handshake closes the connection and never
falls back to plaintext.

HTTP TLS server requirements:

- TLS 1.3 by default, with any broader policy explicit;
- bounded handshake timeout and concurrent pending handshakes;
- one immutable server identity snapshot per accepted connection;
- optional mTLS with required trust anchors and mandatory client-chain
  verification;
- `http/1.1` ALPN only until Forge implements another HTTP protocol;
- the existing body, header, idle, cancellation and shutdown limits after the
  handshake;
- no clear private key in public status, logs or typed diagnostics;
- context rotation for new connections without invalidating established
  sessions.

`forge::net::http::server_config` receives transport-level TLS options or an
immutable TLS context provider. It does not receive Forge Secret ids because a
network library must not depend on a runtime plugin.

### `forge_plugins_net_http_server`

Extend the local-only server API with a constrained asset mount operation:

```cpp
struct asset_mount {
   std::string path = "/admin";
   std::filesystem::path root;
   std::string index = "index.html";
   bool spa_fallback = true;
   std::uint64_t max_file_bytes = 16 * 1024 * 1024;
};

virtual boost::asio::awaitable<void> mount_assets(asset_mount value) = 0;
```

`mount_assets` means register an already built directory such as
`share/product/web` at a configured URL prefix before server startup. It is not
an upload endpoint and does not expose the raw router to product plugins.

The mount record contains only neutral mechanics such as URL prefix, filesystem
root, index file, SPA fallback and cache limits. Product names, frontend
manifests and authorization policy remain downstream-owned. The exact public
record placement is finalized under the `create-plugin` and `create-library`
rules before implementation; the ownership and behavior above are fixed.

A product plugin composes the asset mount and typed API independently:

```cpp
auto http = context.apis().get<forge::plugins::net::http::server::api>(
   forge::plugins::net::http::server::api::ref());

co_await http->mount_assets({
   .path = "/admin",
   .root = "/usr/share/product-admin/web",
   .index = "index.html",
   .spa_fallback = true,
});

co_await http->publish<owner_admin_api>({
   .base_path = "/admin-ui/v1",
});
```

The two prefixes have deliberately different routing domains:

```text
GET  /admin/
     -> /usr/share/product-admin/web/index.html

GET  /admin/assets/app.a81f2.js
     -> /usr/share/product-admin/web/assets/app.a81f2.js

GET  /admin/nodes/validator-1
     -> index.html, then the TypeScript router handles the UI route

GET  /admin-ui/v1/nodes
     -> typed Forge HTTP API

POST /admin-ui/v1/nodes/validator-1/pause
     -> typed Forge HTTP API
```

The SPA fallback is restricted to the asset mount. It cannot consume an
unknown `/admin-ui/v1` API path, and non-GET/HEAD methods never fall back to
`index.html`.

Asset mounting is optional. If nginx, Caddy or an ingress serves the frontend,
the product does not call `mount_assets`; it publishes only `/admin-ui/v1`.
Both paths can still share one browser origin because the reverse proxy serves
`/admin` locally and forwards `/admin-ui/v1` to the native backend. The backend
must not trust forwarded scheme, address or identity headers unless the proxy
source is explicitly configured as trusted.

The HTTP Server plugin also exposes schema-driven TLS policy. A representative
product configuration is:

```yaml
plugins:
  net:
    http:
      server:
        bind-address: 0.0.0.0
        port: 443
        tls:
          mode: server
          certificate-chain-secret: admin-http-certificate
          private-key-secret: admin-http-private-key
          minimum-version: tls1.3
          handshake-timeout-ms: 10000
```

`mode` is `disabled`, `server` or `mutual`. Mutual TLS additionally requires a
trusted client-CA secret. Secret names are configuration values; clear PEM and
private key material are resolved through Forge Crypto Secrets during plugin
lifecycle and are never emitted by config or status APIs. Missing Secrets
support, missing material, malformed PEM or a certificate/key mismatch fail
startup. TLS failure never silently starts a plaintext listener on the
configured port.

The plugin owns an explicit local credential-reload operation. Reload resolves
and validates a complete new identity first, then atomically swaps the context
used by new connections. Existing connections retain their original context.
Partial or invalid rotation leaves the previous identity active and returns a
typed failure.

Middleware responses also gain an append operation or a typed cookie operation
that preserves repeated `Set-Cookie` fields. HTTP requires one header field per
cookie; multiple cookies must not be comma-combined. A successful session flow
may legitimately return, for example:

```text
Set-Cookie: __Host-owner-session=...; Secure; HttpOnly; SameSite=Strict; Path=/
Set-Cookie: __Host-owner-csrf=...; Secure; SameSite=Strict; Path=/
```

The first cookie carries the opaque session credential and is not readable by
JavaScript. The second supports the CSRF double-submit/header check and may be
readable by the same-origin application. A short-lived pre-session pairing
cookie can produce another independent `Set-Cookie` field. The current
middleware `set_header` replacement semantics cannot represent this correctly,
although the lower `forge_net_http` response already preserves repeated cookie
fields.

The plugin continues to reject arbitrary raw-route publication. Typed API
publication, middleware and constrained asset mounting remain the only
supported contribution surfaces.

## Explicitly Unchanged Components

The following libraries are consumers or existing foundations and do not need
an auth-specific API change:

- `forge_api_core` and `forge_api_http`;
- Forge Chain API contracts, raw client, verified client and submission client;
- Forge chain transaction builder and ABI codec;
- Forge keystore and signer providers;
- Forge DB Core, ObjectDB, MDBX and the DB Store plugin;
- Forge Config, App, Asio, Log and OTLP.

If a downstream node-management operation is absent from its existing typed
admin API, that operation is designed separately in the owning API contract. It
must not be smuggled into browser authentication or static-asset libraries.

## Persistence Boundary

Pairing and session libraries own records and valid transitions, not a physical
database. The consuming backend owns persisted models and executes transitions
inside its transaction boundary.

A test-only ObjectDB/MDBX integration exercises this boundary without turning
either auth library into a database wrapper. Concurrent consume, approve and
rotation attempts must yield exactly one successful commit, and the accepted
transition and its audit record must be committed atomically.

The first Spine Admin implementation uses a separate named MDBX/ObjectDB store,
for example `admin`, with neither Revision nor BlobDB unless a concrete model
requires them. It must never reuse the chain `state` or `block` stores.

Atomic product operations include:

- consume bootstrap token and create pending request;
- approve pending request and create owner credential;
- replace pre-session state with an authenticated session;
- rotate or revoke a credential and invalidate its sessions;
- persist the matching security audit record.

This preserves reusable Forge auth logic while keeping product model IDs,
operator roles, retention and audit vocabulary out of Forge.

## Donor Baseline

The pairing behavior is based on the local OpenClaw checkout:

```text
repository: https://github.com/openclaw/openclaw.git
commit:     d6367c2c55a2ef40f189300862d022b0276cc017
```

Reviewed paths include:

- `src/pairing/setup-code.ts`;
- `src/infra/device-bootstrap.ts`;
- `src/infra/device-pairing.ts`;
- `src/infra/pairing-token.ts`;
- `src/infra/pairing-files.ts`;
- `src/infra/json-files.ts`;
- `src/gateway/device-auth.ts`;
- `src/gateway/server/ws-connection/message-handler.ts`;
- `src/gateway/server/ws-connection/handshake-auth-helpers.ts`;
- `src/gateway/server.auth.browser-hardening.test.ts`;
- `ui/src/ui/device-identity.ts`;
- `ui/src/ui/device-auth.ts`;
- `docs/channels/pairing.md`.

Accepted patterns are short-lived bootstrap material, one-time consumption,
bounded pending requests, explicit local approval, supersede on identity or
scope changes, constant-time secret checks, rotation/revocation, atomic private
storage, Origin checks and failed-auth rate limiting.

OpenClaw's long-lived browser device key and token in `localStorage` are not
accepted for this architecture. The native backend owns credentials and gives
the browser an opaque Secure HttpOnly session cookie.

## Security Invariants

- Clear bootstrap/session tokens never enter logs, diagnostics, config output
  or persisted records.
- Browser responses never contain downstream service credentials or private
  signing keys.
- Pairing approval is unavailable through the public browser API.
- A consumed bootstrap token cannot create or modify a second request.
- Scope changes require an explicit transition and cannot silently escalate.
- Session issuance rotates the pre-session identifier.
- State-changing browser methods require an approved session, exact Origin and
  valid CSRF evidence.
- Session, CSRF and pre-session cookies use distinct names and secrets.
- Production browser access requires HTTPS. Plain HTTP is restricted to an
  explicit loopback development policy.
- TLS-enabled startup fails closed when identity or trust material is missing,
  malformed or inconsistent.
- Native TLS and reverse-proxy TLS expose the same authenticated product API;
  neither mode changes session or authorization semantics.
- Assets and API share one origin, but asset paths never bypass API middleware
  or expose arbitrary filesystem files.
- Disabling the embedded asset mount does not weaken API authentication; an
  external static server receives no downstream service credential.
- Product rate limits are applied to issue, begin, complete and authentication
  failures before expensive or state-changing work.

## Delivery Order

1. Extract the reusable `forge_net_tls` context and verification substrate from
   existing STCP mechanics without changing STCP behavior.
2. Add Boost.Beast server-side TLS to `forge_net_http` and schema-driven TLS
   lifecycle to `forge_plugins_net_http_server`.
3. Add `forge.crypto.core` constant-time comparison and migrate Bearer auth.
4. Add `forge_auth_pairing` with deterministic transition and adversarial
   tests.
5. Add `forge_auth_session` with fixation, expiry, rotation and revocation
   tests.
6. Add `forge.net.http.cookie` and repeated `Set-Cookie` preservation.
7. Add `forge_auth_http` and middleware integration tests.
8. Add `forge.net.http.assets` and constrained server-plugin asset mounting.
9. Add package components, relocation consumers, READMEs and donor
   traceability.
10. Integrate the released components into a downstream native admin backend.

Each new library follows the current `create-library` skill. The HTTP server
plugin extension additionally follows `create-plugin`. Aggregate targets,
forwarding modules and compatibility aliases are not introduced.

## Acceptance

Crypto and auth:

- random token uniqueness and fixed entropy requirements;
- constant-time digest comparison used by Bearer, pairing and sessions;
- expiry boundaries and injected-clock tests;
- token replay, concurrent consume, stale approval and superseded request;
- scope escalation rejection, rotation, revocation and session invalidation;
- malformed, oversized and non-canonical token rejection;
- typed Forge exceptions at every public boundary.

HTTP:

- live TLS 1.3 server handshake through Boost.Beast;
- verified hostname and CA HTTPS client against the live Forge server;
- missing certificate, missing key, mismatched identity and malformed PEM;
- plaintext request to a TLS listener and TLS request to a plaintext listener;
- handshake timeout, cancellation, bounded pending handshakes and clean
  shutdown;
- mTLS acceptance only for a client chain rooted in the configured trust
  anchors, plus missing, untrusted and malformed client-certificate rejection;
- certificate context rotation while existing sessions remain valid;
- no plaintext fallback after TLS startup or handshake failure;
- strict cookie parse/format round trips;
- cookie attribute injection and header-splitting rejection;
- multiple `Set-Cookie` fields preserved through middleware and a live server;
- session, CSRF, exact Origin and security-header behavior;
- no CORS wildcard with credentialed browser sessions;
- cancellation, deadlines, body/header limits and failed-auth rate limits.

Assets:

- GET/HEAD, ETag, not-modified and cache policy;
- missing files and SPA fallback;
- traversal, percent-encoding, symlink escape, replacement-race and
  directory-listing rejection;
- MIME allow-list and response-size limits;
- production package relocation with an installed frontend bundle;
- equivalent self-contained and reverse-proxy deployment behavior;
- disabled asset mounting leaves no fallback or filesystem route registered.

Integration:

- one typed workflow API and asset bundle on the same native HTTPS origin;
- equivalent authenticated behavior behind a trusted TLS reverse proxy;
- first-owner pairing through a private local approval boundary;
- restart with durable pending/session state;
- concurrent consume, approve and rotation with one successful ObjectDB/MDBX
  commit and an atomically persisted audit record;
- credential rotation/revocation without restarting the HTTP server;
- downstream typed client remains private to the backend;
- full local CTest, structure/format gates and `git diff --check`.

## Out Of Scope

- A `Forge.Web` frontend framework.
- React, Vue, Vite or any TypeScript build ownership in Forge.
- Product UI, workflow DTOs or node-management policy.
- Transparent proxying of downstream APIs.
- OIDC, JOSE/JWT, JWKS, SSO, multi-user RBAC or password login.
- Browser-held private signing keys or long-lived downstream credentials.
- A new HTTP transport, Chain API client, database engine or crypto vendor.
- HTTP/2, HTTP/3, ACME certificate issuance or a general certificate manager.
