# FORGE Roadmap

FORGE развивается как нейтральный C++23 infrastructure framework и constructor
substrate. Он нужен для сборки распределённых сервисов, DePIN-сетей,
blockchain/control-plane runtimes, P2P сетей, plugin-based daemons and
transport/API layers без переноса продуктовой семантики в FORGE.

После `v1.0.0` основной ориентир — не “дописать старый FC-слой”, а удерживать
чистые reusable boundaries: runtime, config, API contracts, transports, P2P,
plugins, telemetry and compatibility layers должны быть пригодны для серьёзных
потребителей, но не становиться Storlane/Spring/storage/billing runtime.

## Current Framework Surface

- Module-first public API under `libraries/<lib>/include/forge/<lib>/*.cppm`.
- Boost.Describe as canonical reflection metadata.
- `forge_raw` byte compatibility for retained old FC wire layouts.
- Typed `forge_exceptions` categories plus redacted context instead of old
  exception hierarchy.
- Neutral `forge_api_core` contracts for in-process plugin APIs and transport API
  bindings.
- `forge_plugins` aggregate plus focused `forge_plugins_<family>_<name>` targets for shared
  lifecycle-owned components such as P2P nodes, with narrow local APIs for
  route/binding contributions.
- Std chrono instead of old FC time aliases.
- Glaze-backed JSON/YAML codec API.
- Async app/runtime stack over Boost.Asio.
- Separate HTTP, WebSocket, QUIC and P2P libraries.
- Optional Notcurses-backed TUI library.
- Production-grade library READMEs and cross-cutting docs.
- Synchronous logger v2 with structured records, sinks, redaction and private
  stacktrace backend.
- CMake install/export package with external consumer smoke.
- API-over-transport, P2P plugin facade, resolver, diagnostics and PubSub
  plugin flows for typed service composition.
- OTLP log export and crash-spool resend as opt-in observability adapters.
- Planned XML codec API backed by a private lightweight XML backend, followed by
  multi-codec `forge_api_http` binding for XML request/response/error bodies.

## Library Families

- [Runtime + App](runtime/asio-app.md): runtime ownership, scheduler,
  backpressure and async plugin lifecycle.
- [API Contracts](../libraries/api/core/README.md): typed handles, descriptor
  macros, local/remote surfaces, API frames and shared error payloads.
- [HTTP + WebSocket](web/http-websocket.md): web/control-plane substrate,
  routing, middleware, upgrades and retry boundaries.
- [QUIC + P2P](network/quic-p2p.md): secure transport, peer identity, protocol
  streams, relay and path selection.
- [Transport substrate](../libraries/net/transport/README.md): reusable
  stream/session concepts, chunks, frame helpers and muxer substrate.
- [Infrastructure plugins](../plugins/README.md): lifecycle-owned P2P
  node, API resolver, diagnostics and PubSub facade.
- [Telemetry](../libraries/otlp/README.md): opt-in OTLP logs and crash evidence
  export.
- [TUI](tui/notcurses-component-library.md): terminal value models, render
  helpers, navigation and backend isolation.
- [Codecs](codecs/json-yaml-glaze.md): JSON/YAML namespace APIs, Glaze backend
  boundary and diagnostics.
- [XML + HTTP API Codecs](iterations/forge-xml-http-api-codec-v1.md): planned
  XML library first, then HTTP API multi-codec body/error binding.
- [Object Database](iterations/forge-object-database-v1.md): problem notes for
  a neutral typed object/index/session layer above key/value backends.
  Ordered unique/non-unique, composite and persistent ranked indexes with
  transactional count/sum aggregates are active. Hashed indexes are deferred;
  sequenced/random-access do not fit the persisted backend model.
- [DB Revisions v1](iterations/forge-db-revisions-v1.md): implemented separate
  revision library, DB Object system tables, shared Core transaction capture,
  head-only revert and bounded pruning.
- [DB Savepoints v1](iterations/forge-db-savepoints-v1.md): implemented nested
  transaction savepoints, native RocksDB mapping and cross-layer participant
  correctness.
- [DB Revision And Migration Boundary](iterations/forge-db-revisions-migrations-v1.md):
  future migration catalog/runner responsibilities and their optional use of
  revisions and savepoints.
- [DB State Services v1](iterations/forge-db-state-services-v1.md): implemented
  runtime DB Store revision integration, shared read-view tradeoffs, future
  physical checkpoints and deferred generic migrations.
- [Chain Remote State v1](iterations/forge-chain-remote-state-v1.md):
  accepted direction for DB-shaped typed contract-state reads and chain-native
  transaction submission over transport-neutral Forge API contracts. It is not
  a remote DB Core backend; consistency, block anchors, cursors, ABI identity
  and finality remain explicit chain semantics.
- [Config](config/schema-config-program-options.md): schema rules, neutral
  config documents, env/CLI adapters and redaction.
- [CLI Client Foundation v1](iterations/forge-cli-client-foundation-v1.md):
  CLI11-backed nested commands with Forge-owned public types;
  Boost.Program_options remains limited to config argv ingestion.
- [Contract Tooling Family v1](iterations/forge-contract-tooling-family-v1.md):
  host ABI generation, attributes, validation, manifests and contract testing
  live in leaf libraries under `forge.tooling.*`; the guest Contract API and
  Contract SDK identity remain unchanged.
- [VM WASM Backend Family v1](iterations/forge-vm-wasm-backend-family-v1.md):
  accepted clean-breaking split of the current EOS VM lane into
  `forge.vm.wasm.interpret`, reserving sibling `forge.vm.wasm.optimizing` for
  the future EOS VM OC backend without an aggregate VM target.
- [VM WASM Optimizing v1](iterations/forge-vm-wasm-optimizing-v1.md): proposed
  compiler/executor port with isolated compilation, bounded cache, LLVM
  modernization and mandatory first-consumer runtime acceptance.
- [Secret Provider](iterations/fcl-secret-provider-v1.md): neutral
  infrastructure plugin for local secret material, redacted source loading and
  purpose-gated AES-GCM/HKDF operations.
- Historical migration notes: target mapping, raw compatibility,
  Boost.Describe, chrono, exception and logger migration live under
  `docs/migration` and are not current API commitments.

## Release Gates

Build/test gates:

```bash
cmake --build build/forge-debug -j 4

ctest --test-dir build/forge-debug --output-on-failure
git diff --check
```

The default build target is intentional: it builds every configured test
executable before `ctest`, including optional targets when their dependencies
are enabled.

Architecture gates:

- No public `<fc/...>` includes, `namespace fc`, `FC_REFLECT` or `FORGE_REFLECT`.
- No backend parser/terminal/network types in public module interfaces.
- No umbrella target carrying external dependencies "just in case".
- No nested public include directories under `include/forge/<lib>`.
- No absolute local machine paths in docs.
- Every library has a useful README with examples and ownership boundaries.

Security gates:

- Secret-bearing examples use explicit redaction.
- Crypto docs and code paths do not rely on shell-out generation.
- TLS/P2P verification failures are correctness failures.
- UI and HTTP helpers are not documented as authority/security boundaries.

## Ongoing Readiness Work

- Keep library READMEs aligned with public modules and actual targets.
- Keep donor traceability updated when compatibility behavior changes.
- Land XML support before downstream S3-compatible APIs, then extend
  `forge_api_http` instead of allowing product code to bypass typed API binding.
- Re-run package install plus external `find_package(Forge CONFIG REQUIRED)`
  consumer smoke before releases.
- Keep review focused on architecture boundaries, dependency hygiene, security
  and production readiness.

## Out Of Scope For FORGE Core

- Reintroducing source-level `fc::...` compatibility.
- A full schema migration framework in the current DB release surface; the
  future direction is documented separately and is not a shipped API commitment.
- Browser UI or product admin flows.
- Product-specific protocol, storage, billing, authorization or deployment
  semantics.
- Presenting blueprints as shipped public API.
