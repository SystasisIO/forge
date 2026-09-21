# FORGE

FORGE — нейтральный C++23 infrastructure framework и constructor toolkit для
проектов, которые собирают distributed infrastructure: DePIN-сети, то есть
децентрализованные сети физической инфраструктуры, blockchain/control-plane
системы, P2P runtimes, service daemons, plugin-based applications and
transport/API substrates.

FORGE даёт строительные блоки, которые обычно приходится заново писать в каждом
серьёзном продукте: stable serialization, typed configuration, async runtime,
application shell, plugin contracts, API-over-transport, HTTP/WebSocket/QUIC/P2P,
crypto, logging, OTLP export and terminal UI. При этом FORGE не переносит
downstream product vocabulary в публичный API: storage placement, billing,
authorization policy, Storlane/Spring semantics and application protocols живут
выше.

Это уже не source-compatible копия старого FC. Историческая совместимость
сохраняется только там, где она является wire contract: например
`forge::raw::pack` для поддерживаемых типов должен оставаться byte-to-byte
совместимым со старым `fc::raw::pack`. Исходные namespace `fc::...`,
`FC_REFLECT` and старые exception hierarchy не являются частью FORGE API.

## Когда Использовать

- Нужен module-first C++23 framework substrate с явными границами библиотек.
- Нужна бинарная совместимость raw serialization с FC/EOS-like контрактами, но
  без сохранения старого source API.
- Нужна единая схема config flow: `schema -> document -> YAML/JSON/env/CLI -> typed decode`.
- Нужен async runtime поверх Boost.Asio, где shutdown/backpressure являются
  частью API, а не afterthought.
- Нужны neutral HTTP/WebSocket/QUIC/P2P/API/plugin/TUI building blocks для
  DePIN, blockchain and distributed service construction без downstream product
  vocabulary.

## Когда Не Использовать

- Если нужен drop-in replacement для исходников, которые включают `<fc/...>`.
- Если проект не готов к C++23 modules and Homebrew LLVM/modern Clang toolchain.
- Если нужна business-domain layer: FORGE намеренно не знает о продуктовых ролях,
  storage policies, admin flows, billing or application-specific protocols.
- Если нужен browser UI, ORM, DI container or validation framework уровня
  Pydantic: FORGE даёт schema/config diagnostics, но не превращает C++ в web framework.

## Быстрый Старт

```bash
cmake -S . -B build/forge-debug -G Ninja \
  -DBUILD_TESTING=ON \
  -DFORGE_ENABLE_MODULES=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

cmake --build build/forge-debug -j 4
ctest --test-dir build/forge-debug --output-on-failure
```

AppleClang не является целевым компилятором для module build. `import std;` в
baseline не используется; FORGE импортирует свои modules and обычные system/vendor
headers через global module fragment.

## Минимальный Пример

```cpp
#include <boost/describe.hpp>

#include <cstdint>
#include <string>

struct http_config {
   std::string bind_host = "127.0.0.1";
   std::uint16_t bind_port = 8080;
   bool tls_enabled = false;
};

BOOST_DESCRIBE_STRUCT(http_config, (), (bind_host, bind_port, tls_enabled))

import forge.codec.json;
import forge.schema.object;

template <>
struct forge::schema::rules<http_config> {
   static forge::schema::object_schema<http_config> define() {
      auto schema = forge::schema::object<http_config>();
      schema.field<&http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&http_config::bind_port>("bind-port").default_value(8080).range(1, 65535);
      schema.field<&http_config::tls_enabled>("tls-enabled").default_value(false);
      return schema;
   }
};

auto parsed = forge::codec::json::read<http_config>(
   R"({"bind-host":"0.0.0.0","bind-port":9090,"tls-enabled":false})");
if (!parsed.ok()) {
   // typed diagnostics: path, code, severity, message
}
```

## Практические Use Cases

### Typed Config And Codecs

Use `forge_schema` once to describe field names, defaults and validation, then
reuse the same rules from JSON, YAML, XML, environment and CLI adapters. This
keeps diagnostics and redaction consistent across startup paths.

```cpp
auto from_json = forge::codec::json::read<http_config>(json_text);
auto from_xml = forge::codec::xml::read<http_config>(xml_text, {.source_name = "config.xml"});
auto document = forge::config::core::decode_document<http_config>(config_value);
```

### Native HTTP API Binding

Use `forge_api_core` to define a typed contract and `forge_api_http` to publish it
with real HTTP route/path/status semantics. JSON is the default body codec; XML
is opt-in per route for typed DTO bodies.

```cpp
FORGE_HTTP_API(catalog_api,
   FORGE_HTTP_GET(read_item, "/items/:id"),
   FORGE_HTTP_PUT(update_item, "/items/:id", ok,
      FORGE_HTTP_REQUEST_BODY(xml),
      FORGE_HTTP_RESPONSE_BODY(xml)))
```

### Application Plugins

Use `forge_app` and official plugins when several application plugins need one
shared runtime service, such as an HTTP server, a P2P node, a signer, a secrets
service or an OTLP exporter.

```cpp
registry.register_plugin(forge::plugins::http::server::descriptor());
registry.register_plugin(forge::plugins::chain::signer::descriptor());
registry.register_plugin(forge::plugins::crypto::secrets::descriptor());
```

## Библиотеки

| Библиотека | Target | Что Делает | Основные Зависимости |
| --- | --- | --- | --- |
| [core](libraries/core/README.md) | `forge_core` | Strings, UTF-8, type names and `uint128`. | Boost headers/multiprecision as owned implementation detail. |
| [chrono](libraries/chrono/README.md) | `forge_chrono` | ISO-8601/RFC3339 and relative formatting, plus wide nanosecond timestamps. | Boost.DateTime privately. |
| [exceptions](libraries/exceptions/README.md) | `forge_exceptions` | Std-based context errors and capture/assert macros. | `forge_core`. |
| [reflect](libraries/reflect/README.md) | `forge_reflect` | Thin Boost.Describe traversal helpers. | Boost.Describe via Boost headers. |
| [variant](libraries/variant/README.md) | `forge_variant` | Dynamic value/object model and described conversions. | `forge_chrono`, `forge_core`, `forge_reflect`, Boost.MultiIndex/multiprecision. |
| [raw](libraries/raw/README.md) | `forge_raw` | Byte-compatible binary serialization. | `forge_core`, `forge_reflect`, `forge_variant`, `forge_exceptions`. |
| [compression](libraries/compression/README.md) | `forge_compression` | Bounded zlib compression/decompression helpers. | Boost.Iostreams, ZLIB, `forge_exceptions`. |
| [chain/core](libraries/chain/core/README.md) | `forge_chain_core` | Fundamental chain digest and Merkle primitives. | `forge_crypto_digest`, `forge_exceptions`, `forge_raw`. |
| [chain/savanna/values](libraries/chain/savanna/values/README.md) | `forge_chain_savanna_values` | Canonical guest-safe finalizer and finalizer-policy values. | `forge_crypto_bls`, `forge_raw`, `forge_variant`. |
| [chain/protocol](libraries/chain/protocol/README.md) | `forge_chain_protocol` | Canonical protocol values, ordered keys, transactions, blocks, ABI and signing rules. | `forge_chain_core`, `forge_chain_savanna_values`, `forge_compression`, `forge_raw`, `forge_variant`, `forge_crypto_asymmetric_values`, `forge_crypto_digest`. |
| [chain/api](libraries/chain/api/README.md) | `forge_chain_api` | Transport-neutral chain contracts, HTTP/P2P clients and verified audited reads. | `forge_api_core`, `forge_api_http`, `forge_chain_protocol`, `forge_db_authenticated`, `forge_net_http`. |
| [vm/wasm/interpret](libraries/vm/wasm/interpret/README.md) | `forge_vm_wasm_interpret` | Native WebAssembly parser, validator, interpreter, host-function runtime and x86_64 JIT. | `forge_exceptions`, threads, internal SoftFloat. |
| [tooling/abi](libraries/tooling/abi/README.md) | `forge_tooling_abi` | Optional Clang AST based contract ABI and dispatcher generation. | Clang/LLVM privately, `forge_chain_protocol`, `forge_codec_json`. |
| [tooling/attributes](libraries/tooling/attributes/README.md) | `forge_tooling_attributes` | Optional Forge/EOSIO Clang attribute registrations. | Clang/LLVM privately. |
| [tooling/validation](libraries/tooling/validation/README.md) | `forge_tooling_validation` | Optional contract ABI, WASM, import and export validation. | `forge_vm_wasm_interpret`, `forge_chain_protocol`, `forge_codec_json`. |
| [tooling/manifest](libraries/tooling/manifest/README.md) | `forge_tooling_manifest` | Optional deterministic contract build manifests. | `forge_vm_wasm_interpret`, `forge_crypto_digest`, `forge_codec_json`. |
| [tooling/testing](libraries/tooling/testing/README.md) | `forge_tooling_testing` | Optional deterministic VM/ObjectDB host for contract tests. | `forge_vm_wasm_interpret`, `forge_db_object`, contract-visible Forge crypto. |
| [codec/base32](libraries/codec/base32/README.md) | `forge_codec_base32` | Base32 byte/text encoding. | `forge_exceptions`. |
| [json](libraries/codec/json/README.md) | `forge_codec_json` | JSON typed/value/document codec over Glaze. | Glaze privately, `forge_variant`, `forge_config_core`, `forge_schema`. |
| [yaml](libraries/codec/yaml/README.md) | `forge_codec_yaml` | YAML typed/value/document codec with JSON-shaped API. | Glaze privately, `forge_config_core`, `forge_schema`. |
| [xml](libraries/codec/xml/README.md) | `forge_codec_xml` | XML typed/tree codec over private pugixml. | `forge_core`, `forge_reflect`, `forge_schema`, pugixml privately. |
| [schema](libraries/schema/README.md) | `forge_schema` | Field rules, defaults, ranges, diagnostics. | `forge_reflect`. |
| [config](libraries/config/core/README.md) | `forge_config_core` | Neutral config document, merge, decode, redaction. | `forge_schema`. |
| [program_options](libraries/config/program_options/README.md) | `forge_config_program_options` | CLI adapter from Boost.Program_options into config documents. | Boost.Program_options privately. |
| [env](libraries/config/env/README.md) | `forge_config_env` | Process env and explicit `.env` adapter into config documents. | `forge_config_core`, `forge_schema`. |
| [api/core](libraries/api/core/README.md) | `forge_api_core` | Typed local/remote API contracts, handles, descriptors and frame vocabulary. | `forge_exceptions`, `forge_raw`. |
| [api/auth](libraries/api/auth/README.md) | `forge_api_auth` | Trusted transport caller identity and required server-supplied injection. | `forge_api_core`, `forge_crypto_digest`, `forge_raw`. |
| [api/stream](libraries/api/stream/README.md) | `forge_api_stream` | Server-side API frame loop over reusable transport streams. | `forge_api_core`, `forge_raw`, `forge_net_transport`. |
| [api/transport](libraries/api/transport/README.md) | `forge_api_transport` | Generic API transport client, connection and session serving. | `forge_api_stream`, `forge_net_transport`. |
| [api/http](libraries/api/http/README.md) | `forge_api_http` | Typed Forge API contracts over native HTTP routes with JSON/XML codecs. | `forge_net_http`, `forge_api_core`, `forge_codec_json`, `forge_codec_xml`. |
| [api/quic](libraries/api/quic/README.md) | `forge_api_quic` | API frame binding over QUIC streams. | `forge_api_stream`, `forge_net_quic`. |
| [api/websocket](libraries/api/websocket/README.md) | `forge_api_websocket` | API frame binding over WebSocket messages. | `forge_api_core`, `forge_net_websocket`, `forge_raw`. |
| [api/p2p](libraries/api/p2p/README.md) | `forge_api_p2p` | API frame binding over negotiated P2P protocol streams. | `forge_api_stream`, `forge_net_p2p`. |
| [crypto/core](libraries/crypto/core/README.md) | `forge_crypto_core` | Byte ownership, secret memory and secure random data. | `forge_exceptions`, OpenSSL::Crypto. |
| [crypto/digest](libraries/crypto/digest/README.md) | `forge_crypto_digest` | Digests, HMAC and Raw pack hashing. | `forge_crypto_core`, `forge_raw`, `forge_variant`, OpenSSL::Crypto. |
| [crypto/symmetric](libraries/crypto/symmetric/README.md) | `forge_crypto_symmetric` | AES, ChaCha20-Poly1305, HKDF and scrypt. | `forge_crypto_core`, `forge_exceptions`, OpenSSL::Crypto. |
| [crypto/asymmetric](libraries/crypto/asymmetric/README.md) | `forge_crypto_asymmetric_values`, `forge_crypto_asymmetric` | Binary key/signature values and host signing algorithms. | `forge_raw`; host algorithms add OpenSSL and secp256k1. |
| [crypto/signer](libraries/crypto/signer/README.md) | `forge_crypto_signer` | Low-level named asymmetric signing-provider contract and configured provider. | `forge_crypto_asymmetric`. |
| [crypto/pki](libraries/crypto/pki/README.md) | `forge_crypto_pki` | DER, PEM and X.509 boundaries. | `forge_crypto_asymmetric`, `forge_crypto_digest`, OpenSSL::Crypto. |
| [crypto/math](libraries/crypto/math/README.md) | `forge_crypto_math` | Big integers and modular arithmetic. | `forge_crypto_core`, OpenSSL::Crypto, GMP. |
| [crypto/bls](libraries/crypto/bls/README.md) | `forge_crypto_bls` | BLS values, signatures and contract primitives. | `forge_crypto_digest`, BLS12-381, OpenSSL::Crypto. |
| [crypto/bls/signer](libraries/crypto/bls/signer/README.md) | `forge_crypto_bls_signer` | Low-level BLS provider contract and configured provider. | `forge_crypto_bls`, `forge_exceptions`. |
| [crypto/bn256](libraries/crypto/bn256/README.md) | `forge_crypto_bn256` | BN254 operations. | Internal BN256 backend. |
| [log](libraries/log/README.md) | `forge_log` | Logging core, messages, console/appender boundary. | `forge_chrono`, `forge_variant`, Boost.DLL privately. |
| [otlp](libraries/otlp/README.md) | `forge_otlp` | OTLP/HTTP JSON log export and crash-spool resend. | `forge_log`, `forge_net_http`, `forge_asio`. |
| [asio](libraries/asio/README.md) | `forge_asio` | Asio runtime, priority task scheduler and bounded CPU compute pool. | Boost.Asio, threads. |
| [app](libraries/app/README.md) | `forge_app` | Opinionated application shell, plugins, ports, config and diagnostics. | `forge_asio`, `forge_config_core`. |
| [auth/oauth2](libraries/auth/oauth2/README.md) | `forge_auth_oauth2` | Client-side access-token contracts, interactive authorization and typed OAuth errors. | `forge_crypto_core`, `forge_exceptions`. |
| [auth/appauth](libraries/auth/appauth/README.md) | `forge_auth_appauth` | Apple Authorization Code + PKCE backend using AppAuth, the system browser and Keychain. | `forge_auth_oauth2`, AppAuth 3.0.0 privately. |
| [auth/oidc_agent](libraries/auth/oidc_agent/README.md) | `forge_auth_oidc_agent` | Linux/headless token acquisition through the public oidc-agent C API. | `forge_auth_oauth2`, `forge_asio`, oidc-agent 5.3.8 privately. |
| [auth/workload](libraries/auth/workload/README.md) | `forge_auth_workload` | Bounded workload subject-token acquisition and hardened projected-token files. | `forge_asio`, `forge_crypto_core`, `forge_exceptions`. |
| [auth/pairing](libraries/auth/pairing/README.md) | `forge_auth_pairing` | Product-neutral bootstrap, pairing and credential state transitions with canonical persisted Raw serialization. | `forge_codec_base64`, `forge_crypto_core`, `forge_crypto_digest`, `forge_exceptions`, `forge_raw`. |
| [auth/session](libraries/auth/session/README.md) | `forge_auth_session` | Product-neutral digest-only session, CSRF and credential-binding transitions with canonical persisted Raw serialization. | `forge_auth_pairing`, `forge_codec_base64`, `forge_crypto_core`, `forge_crypto_digest`, `forge_exceptions`, `forge_raw`. |
| [auth/http](libraries/auth/http/README.md) | `forge_auth_http` | Browser session evidence, exact Origin/CSRF policy, cookie emission and security headers. | `forge_auth_session`, `forge_net_http`. |
| [net/http](libraries/net/http/README.md) | `forge_net_http` | HTTP target/base URL, strict cookies, hardened streamed assets, router, middleware, client/server with optional fixed-mode TLS listener. | `forge_net_tls`, `forge_net_websocket`, Boost.Beast/URL/Asio. |
| [net/tls](libraries/net/tls/README.md) | `forge_net_tls` | Immutable TLS context snapshots, trust loading and peer verification. | `forge_exceptions`, `forge_crypto_pki`, Boost.Asio SSL, Boost.Beast, OpenSSL. |
| [net/websocket](libraries/net/websocket/README.md) | `forge_net_websocket` | WebSocket connection/client primitives. | `forge_net_tls`, Boost.Beast/Asio, OpenSSL. |
| [net/transport](libraries/net/transport/README.md) | `forge_net_transport` | Reusable stream/session concepts, chunk buffers and frame helpers. | Boost.Asio, `forge_exceptions`. |
| [net/tcp](libraries/net/tcp/README.md) | `forge_net_tcp` | TCP transport adapter over `forge_net_transport`. | Boost.Asio, `forge_net_transport`. |
| [net/stcp](libraries/net/stcp/README.md) | `forge_net_stcp` | Secure TCP transport profile. | `forge_net_tcp`, `forge_net_tls`, `forge_crypto_pki`, `forge_net_transport`. |
| [net/yamux](libraries/net/yamux/README.md) | `forge_net_yamux` | Yamux multiplexed sessions over a transport stream. | `forge_net_transport`, Boost.Asio. |
| [net/quic](libraries/net/quic/README.md) | `forge_net_quic` | QUIC endpoint, listener, connector, framed streams. | ngtcp2, OpenSSL 3.0+, Boost.Asio. |
| [multiformats](libraries/multiformats/README.md) | `forge_multiformats` | libp2p-compatible varint, multicodec, multihash, multibase and multiaddr. | `forge_codec_base32`, `forge_codec_base58`, `forge_crypto_digest`, `forge_exceptions`. |
| [net/p2p](libraries/net/p2p/README.md) | `forge_net_p2p` | Peer identity, sessions, wire Ping, discovery, relay, DHT, rendezvous and GossipSub; host-local reachability policy is a separate Stage 6 surface. | `forge_net_transport`, `forge_multiformats`, `forge_net_quic`, `forge_net_yamux`. |
| [db/ids](libraries/db/ids/README.md) | `forge_db_ids` | Compact database object IDs and guest-safe typed ID bindings. | `forge_raw`, `forge_variant`, `forge_exceptions`. |
| [db/core](libraries/db/core/README.md) | `forge_db_core` | Shared record driver, transaction and snapshot contract. | Boost.Asio, `forge_exceptions`. |
| [db/object](libraries/db/object/README.md) | `forge_db_object` | Typed object/index store over the shared DB driver. | Boost.Asio, `forge_db_core`, `forge_db_ids`, `forge_raw`, `forge_exceptions`. |
| [db/blob](libraries/db/blob/README.md) | `forge_db_blob` | Content-addressed blob store with typed refs and explicit retention primitives. | Boost.Asio, `forge_db_core`, `forge_crypto_digest`, `forge_raw`, `forge_variant`, `forge_exceptions`. |
| [db/revision](libraries/db/revision/README.md) | `forge_db_revision` | Durable before-image revisions with strict-head revert and bounded whole-revision prune. | Boost.Asio, `forge_db_core`, `forge_db_object`, `forge_raw`, `forge_exceptions`. |
| [db/authenticated](libraries/db/authenticated/README.md) | `forge_db_authenticated` | Experimental persistent authenticated ordered state and transferable proofs. | Boost.Asio, `forge_crypto_digest`, `forge_db_core`, `forge_exceptions`. |
| [db/mdbx](libraries/db/mdbx/README.md) | `forge_db_mdbx` | Vendored libmdbx implementation of the shared DB driver contract. | `forge_asio`, `forge_db_core`, `forge_exceptions`; libmdbx privately. |
| [rocksdb](libraries/rocksdb/README.md) | `forge_rocksdb` | Optional RocksDB TransactionDB wrapper. | RocksDB privately, `forge_exceptions`, `forge_schema`. |
| [db/rocksdb](libraries/db/rocksdb/README.md) | `forge_db_rocksdb` | RocksDB implementation of the shared DB driver contract. | `forge_db_core`, `forge_rocksdb`. |
| [plugins](plugins/README.md) | `forge_plugins`, `forge_plugins_*_*` | Official infrastructure plugins: Chain signer, crypto secrets, P2P node/resolver/diagnostics/PubSub, HTTP server, logging and named DB Store. | `forge_app`, `forge_api_core`, focused plugin targets. |
| [tui](libraries/tui/README.md) | `forge_tui` | Terminal UI value models, render helpers, runner. | Notcurses core privately and optionally. |

`find_package(Forge CONFIG REQUIRED)` is intentionally lightweight and discovers
only the `core` package surface. Production code that needs feature libraries
must request components and then link concrete leaf targets such as
`Forge::forge_config_core`, `Forge::forge_config_env`, `Forge::forge_codec_json`,
`Forge::forge_codec_xml`, `Forge::forge_api_http` or `Forge::forge_net_quic`. External backends like
OpenSSL, ngtcp2, Glaze and Boost components belong to the leaf target that
actually owns their API or implementation use. `Forge::forge` remains the all-in
aggregate target, but consumers should request `COMPONENTS all` before linking
it.

Contract development is distributed separately from the ordinary host package.
The standalone [guest SDK](guest/README.md) builds the pinned wasm32 sysroot,
guest runtime, modern contract API, EOSIO compatibility veneer and thin tools.
Experimental dual-target contract libraries use ordinary CMake targets in
independent native and wasm32 configurations. Products share physical sources
with `add_subdirectory()`; Forge adds guest compile settings, ABI generation and
the runtime artifact manifest without defining a second build graph or source
package format. Host applications may request the optional `tooling_*`
components above without installing the guest sysroot; only `tooling_abi` and
`tooling_attributes` require a compatible Clang package.

## Архитектурные Документы

- [docs/index.md](docs/index.md) — карта документации.
- [docs/roadmap.md](docs/roadmap.md) — release readiness and migration gates.
- [docs/runtime/asio-app.md](docs/runtime/asio-app.md) — runtime, scheduler and async app lifecycle.
- [docs/web/http-websocket.md](docs/web/http-websocket.md) — HTTP/WebSocket layering.
- [docs/network/quic-p2p.md](docs/network/quic-p2p.md) — QUIC and P2P model.
- [docs/iterations/forge-p2p-production-implementation-v1.md](docs/iterations/forge-p2p-production-implementation-v1.md) — Stage 6 P2P ownership and canonical runner acceptance promotion.
- [docs/blueprints/blockchain-constructor/README.md](docs/blueprints/blockchain-constructor/README.md) — planning map for FORGE as a neutral constructor substrate.
- [docs/tui/notcurses-component-library.md](docs/tui/notcurses-component-library.md) — TUI abstraction over Notcurses.
- [docs/codecs/json-yaml-glaze.md](docs/codecs/json-yaml-glaze.md) — JSON/YAML codec boundary.
- [docs/config/schema-config-program-options.md](docs/config/schema-config-program-options.md) — schema/config/CLI flow.

README в `libraries/<lib>` является быстрым guide по конкретной библиотеке.
`/docs` хранит только сквозные решения, которые проходят через несколько
библиотек.

## Версионирование

FORGE использует версию `MAJOR.MINOR.PATCH` вместе с явным статусом контрактов:

- `Stable` является статусом по умолчанию; его несовместимое изменение требует
  нового `MAJOR` release.
- `Preview` и `Experimental` должны быть явно отмечены в README владельца.
  Их source API может документированно меняться в `MINOR` release.
- `PATCH` release не содержит намеренных несовместимых изменений.
- До объявления source-контрактов Forge стабилизированными maintainer может
  разрешить один явно ограниченный source/package break в `MINOR` release.
  Release notes обязаны перечислить удалённые поверхности и механический путь
  миграции; wire и persisted storage formats этим исключением не ослабляются.
- Wire и persisted storage contracts оцениваются отдельно: нестабильность C++
  API сама по себе не разрешает менять байты или сохранённые данные.

Каждое разрешённое несовместимое изменение Preview/Experimental API должно быть
описано в release notes вместе с migration path.

Текущие изменения и переходы описаны в
[Forge 8.29.1 release notes](docs/releases/8.29.1.md).

## Совместимость

- `forge::raw::pack/unpack` сохраняет старый byte layout для retained primitive,
  chrono, string/container, variant/static_variant, described object and crypto
  wrapper cases, покрытых golden tests.
- Reflection canonical spelling — Boost.Describe. `FORGE_REFLECT` and `FC_REFLECT`
  запрещены.
- Time API использует `std::chrono`; старые `forge::time_point` aliases не
  возвращаются. Узкое исключение: `forge::chrono::timestamp` хранит широкую
  временную метку с наносекундной точностью, без часов и `now()`.
- Ошибки являются std-compatible: `context_error` используется только для
  structured context and nested exception chains.

## Security Baseline

- Secrets must be redacted before logging, JSON/YAML output or TUI rendering.
- Crypto generation and verification are in-process; shell-out crypto не является
  допустимым product path.
- TLS/QUIC/P2P verification errors are typed failures, not generic connection
  messages.
- UI and HTTP helpers are not authority boundaries. Authorization and signing
  decisions belong to the consuming product.

## Release Gates

```bash
cmake --build build/forge-debug -j 4

ctest --test-dir build/forge-debug --output-on-failure
git diff --check
```

The default build target is intentional: it builds every configured test
executable before `ctest`, including optional targets when their dependencies
are enabled.

Static gates used during development:

```bash
rg "#include\\s*[<\"]fc/|namespace fc\\b|fc::|FC_REFLECT|FORGE_REFLECT" libraries tests CMakeLists.txt cmake
find libraries -path '*/include/forge/*/*' -type d -print
rg "glz::|YAML::Node|notcurses" libraries/*/include -g '*.cppm'
```

Expected result: no product hits, except explicitly documented macro-only
headers such as `libraries/exceptions/include/forge/exceptions/macros.hpp`.

## Authenticated DB Performance Acceptance

`benchmark_forge_db_authenticated` always writes one JSON document to stdout.
An invocation without `--baseline` is a measurement-only 10K-key smoke run and
does not enforce a gate. The explicit `1m` and `10m` profiles enforce these
provisional throughput floors:

| Profile | Keys | Initial batch | Point proofs | Ranked range proofs |
| --- | ---: | ---: | ---: | ---: |
| `1m` | 1,000,000 | 250 keys/s | 2 proofs/s | 0.2 proofs/s |
| `10m` | 10,000,000 | 250 keys/s | 2 proofs/s | 0.2 proofs/s |

These are deliberately broad measurement guardrails for detecting stalled or
grossly regressed runs. They are not supported latency SLOs or product promises.
Proof latency, encoded size and node-count fields remain measurements until
representative ARM64 and Linux x86_64 baselines have been collected. The JSON
records `format`, `machine_label`, observed metrics, thresholds, per-check
booleans and `acceptance.status`; a failed profile exits with status `2`.

Both CTest registrations are off by default, run serially and have explicit
timeouts. Configure only the scale that the machine is intended to measure:

```bash
cmake -S . -B build/forge-authenticated-acceptance -G Ninja \
  -DBUILD_TESTING=ON \
  -DFORGE_ENABLE_MODULES=ON \
  -DFORGE_DB_AUTHENTICATED_ENABLE_1M_PERFORMANCE_TEST=ON \
  -DFORGE_DB_AUTHENTICATED_ENABLE_10M_PERFORMANCE_TEST=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

cmake --build build/forge-authenticated-acceptance \
  --target benchmark_forge_db_authenticated -j 4

ctest --test-dir build/forge-authenticated-acceptance \
  -R '^test_forge_db_authenticated_performance_(1m|10m)$' \
  --output-on-failure -V
```

For clean machine-readable artifacts, invoke the target directly:

```bash
mkdir -p build/forge-authenticated-acceptance/results

./build/forge-authenticated-acceptance/tests/benchmark_forge_db_authenticated \
  --baseline 1m --machine-label "$(uname -s)-$(uname -m)" \
  > build/forge-authenticated-acceptance/results/authenticated-1m.json

./build/forge-authenticated-acceptance/tests/benchmark_forge_db_authenticated \
  --baseline 10m --machine-label "$(uname -s)-$(uname -m)" \
  > build/forge-authenticated-acceptance/results/authenticated-10m.json
```

The authenticated proof fuzzer remains a separate opt-in target. Its single
[`fuzz_driver.cpp`](tests/db_authenticated/fuzz_driver.cpp) dispatches both point
and ranked-range inputs; CMake compiles that driver with libFuzzer, ASan (Address
Sanitizer) and UBSan (UndefinedBehaviorSanitizer) together. ASan/UBSan CI lanes
therefore consume the same corpus and binary, while varying runtime budget and
environment policy rather than maintaining divergent drivers:

```bash
cmake -S . -B build/forge-authenticated-fuzz -G Ninja \
  -DBUILD_TESTING=ON \
  -DFORGE_ENABLE_MODULES=ON \
  -DFORGE_DB_AUTHENTICATED_ENABLE_FUZZ_TESTS=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

cmake --build build/forge-authenticated-fuzz \
  --target forge_db_authenticated_fuzz -j 4

mkdir -p build/forge-authenticated-fuzz/corpus \
  build/forge-authenticated-fuzz/artifacts

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build/forge-authenticated-fuzz/tests/forge_db_authenticated_fuzz \
  build/forge-authenticated-fuzz/corpus \
  -max_total_time=900 -timeout=10 -rss_limit_mb=4096 \
  -artifact_prefix=build/forge-authenticated-fuzz/artifacts/
```

## Install And Consume

```bash
cmake --install build/forge-debug --prefix build/forge-install --component dev
```

Consumer CMake:

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS raw crypto_digest app log api_http codec_xml)

target_link_libraries(my_program PRIVATE
   Forge::forge_raw
   Forge::forge_crypto_digest
   Forge::forge_app
   Forge::forge_log
   Forge::forge_api_http
   Forge::forge_codec_xml
)
```

The repository also contains external smoke projects under
[`tests/package_consumer`](tests/package_consumer) and
[`tests/package_core_lightweight`](tests/package_core_lightweight). They verify
both component-rich consumers and the lightweight `find_package(Forge)` path that
does not discover heavy crypto/transport/codec backends.

## License

FORGE is licensed under the Apache License, Version 2.0.

Copyright (c) 2026 Vladimir Tarnakin.

FORGE is intended to be a reusable infrastructure framework for open-source and
commercial software. Downstream products may use separate licensing terms for
their product layers.

Canonical upstream:
https://github.com/crossfed/forge
