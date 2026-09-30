# HTTP Server Plugin

`forge::plugins::net::http::server` owns one configured `forge_net_http` server and exposes a
local-only API for application plugins to publish typed HTTP APIs and middleware
before startup. It composes the server's TLS identity from the local Crypto
Secrets API without exposing PEM material to application plugins.

## When To Use

- A `forge_app` daemon needs one shared HTTP server configured through the
  plugin config tree.
- Several application plugins need to contribute typed HTTP APIs or middleware
  before server startup.
- You want native HTTP route/path/status semantics without each product plugin
  owning sockets and lifecycle.

## When Not To Use

- Do not use this plugin for raw socket ownership or transport experiments. Use
  `forge_net_http` directly for low-level HTTP mechanics and explicit TLS contexts.
- Do not put product authorization, account policy, storage policy or protocol
  vocabulary into this plugin.
- Do not publish routes after the server has entered startup; contribution
  windows are lifecycle-bound.

## Identity

- Target: `forge_plugins_net_http_server`
- Package component: `plugins_net_http_server`
- Plugin id: `forge.plugins.net.http.server`
- Main API id: `forge.plugins.net.http.server`
- API contract: `2.0`
- Config section: `plugins.net.http.server`
- Public modules:
  - `forge.plugins.net.http.server.plugin`
  - `forge.plugins.net.http.server.descriptor`
  - `forge.plugins.net.http.server.api`
  - `forge.plugins.net.http.server.bearer_auth`
  - `forge.plugins.net.http.server.middleware`
  - `forge.plugins.net.http.server.types`
  - `forge.plugins.net.http.server.exceptions`

## What It Provides

- Starts and stops one HTTP server through the `forge_app` lifecycle.
- Applies schema-driven server config: bind address, port, base path, body/header
  limits, timeouts and TLS listener policy.
- Accepts typed `FORGE_HTTP_API` publications through `publish<Interface>()`.
- Accepts plugin-owned middleware descriptors through
  `forge::plugins::net::http::server::middleware_descriptor`.
- Accepts bounded browser asset mounts through `api::mount_assets(asset_mount)`.
- Provides a reusable Bearer authentication middleware that compares SHA-256
  token hashes in constant time and never stores the clear token in its options.
- Reloads a TLS server identity through `api::reload_tls()` after complete new
  secret material has been resolved and validated.

It does not expose raw route verbs, raw `forge::net::http::router`,
diagnostics/status endpoints, product authorization policy, CORS policy or
product-specific behavior.

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_net_http`
- `forge_net_tls`
- `forge_api_http`
- `forge_config_core`
- `forge_schema`
- `forge_plugins_crypto_secrets` for TLS-enabled runtime composition
- Boost.Asio

## Config

```yaml
plugins:
   net:
     http:
        server:
           bind-address: 127.0.0.1
           port: 8080
           api-base-path: /api/v1
           max-request-body-bytes: 16777216
           max-header-bytes: 65536
           read-timeout-ms: 30000
           idle-timeout-ms: 120000
           tls:
              mode: disabled
              handshake-timeout-ms: 10000
              max-pending-handshakes: 64
```

`api-base-path` is the default base path for published typed APIs. A
publication can override it with `publish_options::base_path`.

Plaintext `mode: disabled` is accepted only for `127.0.0.1`, `::1` or the
explicit `localhost` spelling, which binds `127.0.0.1`. Wildcard and public
addresses are rejected. TLS modes are `server` and `mutual`:

```yaml
tls:
   mode: mutual
   certificate-chain-secret: admin-http-certificate
   private-key-secret: admin-http-private-key
   client-ca-secret: admin-http-client-ca
   handshake-timeout-ms: 10000
   max-pending-handshakes: 64
```

TLS modes resolve the three distinct purposes `http.server.tls.certificate-chain`,
`http.server.tls.private-key` and `http.server.tls.client-ca` through
`forge.plugins.crypto.secrets::api::get_bytes`. Mutual mode requires an explicit
client CA and does not fall back to system trust. Missing API support, denied or
malformed material, and certificate/key mismatch fail startup without opening a
plaintext listener. `reload_tls()` resolves every configured secret into a new
context first, then performs one atomic provider replacement; a failed reload
keeps the active context unchanged.

## Examples

### Catalog API

This example demonstrates typed HTTP route publication, a native path parameter,
an XML request/response route and middleware. It intentionally uses neutral
catalog vocabulary; protocol-specific route matrices and policy belong in the
consumer.

```cpp
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

import forge.api.core.binding;
import forge.app.plugin;
import forge.net.http.file;
import forge.net.http.stream;
import forge.net.http.types;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.bearer_auth;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.plugin;

struct item_request : forge::net::http::endpoint_request {
   std::string id;
};

struct update_item_request : forge::net::http::endpoint_request {
   std::string id;
   std::string title;
};

struct item_response {
   std::string id;
   std::string title;
};

BOOST_DESCRIBE_STRUCT(item_request, (), (id))
BOOST_DESCRIBE_STRUCT(update_item_request, (), (id, title))
BOOST_DESCRIBE_STRUCT(item_response, (), (id, title))

class catalog_api : public forge::api::core::contract<catalog_api> {
 public:
   virtual boost::asio::awaitable<item_response>
   read_item(item_request request) = 0;

   virtual boost::asio::awaitable<item_response>
   update_item(update_item_request request) = 0;
};

FORGE_API(catalog_api,
   FORGE_API_CONTRACT("example.catalog", 1, 0),
   FORGE_API_METHOD(read_item),
   FORGE_API_METHOD(update_item))

FORGE_HTTP_API(catalog_api,
   FORGE_HTTP_GET(read_item, "/items/:id", ok),
   FORGE_HTTP_PUT(update_item, "/items/:id", ok,
      FORGE_HTTP_REQUEST_BODY(xml),
      FORGE_HTTP_RESPONSE_BODY(xml),
      FORGE_HTTP_ERROR_BODY(xml)))
```

The application plugin only contributes the typed API and middleware. The HTTP
server plugin applies the configured server lifecycle and mounts the route
mapping under the configured base path.

An application plugin can also contribute a bounded browser asset mount without
receiving the raw router. Asset mounts serve only GET/HEAD through the native
file/range stream path, and cannot overlap a typed API base path. Use disjoint
prefixes such as `/admin-ui/v1` for APIs and `/admin` for browser files.

```cpp
import forge.net.http.assets;

auto http = context.apis().get<forge::plugins::net::http::server::api>(
   {.id = {"forge.plugins.net.http.server"}, .major = 2});
co_await http->mount_assets(forge::net::http::asset_mount{
   .path = "/admin",
   .root = configured_admin_ui_directory,
   .index = "index.html",
   .spa_fallback = true,
   .max_file_bytes = 16 * 1024 * 1024,
});
```

Sensitive typed APIs can require mTLS at publication with
`publish_options{.require_mutual_tls = true}`. Registration fails when the
configured listener is plaintext or server-authenticated TLS only. This does
not upgrade a shared public listener silently; use a separate service or
configure mutual TLS explicitly before publishing such APIs.

The HTTP Server plugin injects the application-owned `context.compute()`
executor when it builds the asset bundle. Applications that publish assets must
configure that bounded compute pool; a missing executor fails publication before
the listener starts. `asset_mount` itself remains a simple configuration DTO.

```cpp
class object_http_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return {.value = "object.http"};
   }

   [[nodiscard]] std::string version() const override {
      return "1.0.0";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<forge::plugins::net::http::server::api>(
         {.id = {"forge.plugins.net.http.server"}, .major = 2});

      co_await http->use(forge::plugins::net::http::server::bearer_auth({
         .path_prefix = "/api/v1/admin",
         .token_hashes = {
            forge::plugins::net::http::server::hash_bearer_token(configured_token),
         },
      }));

      co_await http->use(forge::plugins::net::http::server::middleware_descriptor{
         .id = "trace",
         .phase = forge::plugins::net::http::server::middleware_phase::before_handler,
         .order = 10,
         .path_prefix = "/api",
         .handler = [](const forge::plugins::net::http::server::middleware_request&,
                       forge::plugins::net::http::server::middleware_next next)
            -> boost::asio::awaitable<forge::plugins::net::http::server::middleware_response> {
            auto response = co_await next();
            response.set_header("X-Trace", "enabled");
            co_return response;
         },
      });

      co_await http->publish<catalog_api>(
         forge::plugins::net::http::server::publish_options{.base_path = "/api/v1"});
   }
};
```

Register the infrastructure plugin once:

```cpp
registry.register_plugin(forge::plugins::net::http::server::descriptor());
```

## Security And Boundaries

- The plugin is not an authority boundary. Authentication, authorization,
  tenancy and rate policy belong to middleware or consuming product plugins.
- `bearer_auth` proves possession of a configured token only. The consumer
  chooses protected paths and maps authenticated callers to product permissions.
- Prefer supplying precomputed token hashes from a secret provider. The helper
  `hash_bearer_token` is intended for already-secret runtime input, not for
  embedding clear production tokens in source or ordinary config files.
- Body/header limits and timeouts are config-owned and enforced by `forge_net_http`.
- TLS Secret IDs are identifiers, not PEM values. TLS-enabled startup and reload
  never log exported secret bytes.
- Middleware should avoid logging raw headers, query strings or request bodies
  before redaction.
- Native file/stream responses are route-level escape hatches; they do not go
  through JSON/XML DTO codecs.

## Common Mistakes

- Publishing an API after startup. Publish from application plugin
  initialization.
- Adding product-specific route helpers to this plugin instead of using typed
  `FORGE_HTTP_API` mappings.
- Treating `api-base-path` as a security boundary. It is only route mounting.
- Mounting assets under a typed API prefix. This is rejected before the listener
  starts instead of silently making either surface unreachable.
- Reimplementing HTTP server lifecycle in each application plugin instead of
  sharing this plugin.

## Tests

- `test_forge_plugins`
- `test_forge_plugins_net_http_server`
- `test_forge_http_websocket`
- `test_forge_api_core`
