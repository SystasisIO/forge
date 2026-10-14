# HTTP Server Observability

## Inspected Sources

Existing local donors were inspected, not fetched, built or modified. Paths are
workspace-relative; the named Spring/EOSIO files are clean against these heads.

| Donor | Revision | Inspected files |
| --- | --- | --- |
| Spring | `e6a99f68b67abc4d89fe716755b2e1394a4991f7` | `plugins/http_plugin/http_plugin.cpp` (startup, route registration, error classification); `plugins/http_plugin/include/eosio/http_plugin/beast_http_session.hpp` (session errors and in-flight limits) |
| EOSIO | `11d35f0f934402321853119d36caeb7022813743` | `plugins/http_plugin/http_plugin.cpp` (TLS setup, 429 limits, listener startup, request errors) |
| Local BitShares/Graphene-derived `bitstore-core` | pinned Git blob `738940ea7a93a342ad9d0883d4595ced3258470b` | `libraries/app/application.cpp`, `reset_websocket_server()` and shutdown boundaries; this is a local renamed derivative, not verified upstream BitShares revision mapping |

## Adopted And Rejected Patterns

Adopt significant listener and TLS setup/teardown records, typed failure
classification, and separate diagnostic logging for ordinary request rejection.
Spring/EOSIO resource-limit records expose counts rather than entire request
objects; keep that separation. Emit listener readiness only after the actual
bind/listen succeeds, not before it as some donor messages do.

Do not copy donor `what()`, `to_detail_string()`, bad-request bodies, raw request
targets, host headers or config dumps. Forge records only fixed events,
allowlisted operation/outcome/category names and bounded numeric context. A 4xx
or normal disconnect is not an unexpected server error. Untrusted repeated
failures must not generate unbounded WARNING/ERROR output.

Do not copy destructor-driven cleanup, introduce an HTTP exporter, create a
second runtime, or mirror lifecycle state for logging. The server records actual
transport outcomes; App already records plugin lifecycle. TLS reload logging
belongs to the plugin that composes Secrets and publishes the replacement.

The optional access record is emitted once at HTTP completion/abort, not at
handler return and not per streamed chunk. Successfully writing a response does
not prove remote application consumption. No raw URL, peer identity, headers,
payload or client correlation field is needed for this slice. HTTP/WebSocket
protocol semantics and authorization remain unchanged.

## Verification Boundary

The [iteration plan](../iterations/forge-http-observability-v1.md) defines focused
native, package and review gates. Source inspection alone is not a PASS receipt.
Existing Forge log routing can deliver records to OTLP; this change does not
claim collector availability or delivery before exporter startup/after shutdown.
