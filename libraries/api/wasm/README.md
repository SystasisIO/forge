# WASM Host/Guest Protocol

Status: **Preview**. Component `api_wasm`, target `Forge::forge_api_wasm`,
namespace `forge::api::wasm`. This is an owned-value host/guest protocol, not
a network transport or an action executor. Native and SDK guest builds compile
the same module sources. There is no `host`, `guest`, or `values` namespace.

## Values And Ownership

| Module suffix | Owned values |
|---|---|
| `payload` | Explicit type name and canonical `forge::raw::bytes` |
| `continuation` | Version and typed state |
| `effect` | Named binding, typed request, whole response type, requested delay |
| `step` | `awaiting`, `completed`, and their variant with canonical API error |
| `event` | Typed successful result or canonical API error |
| `context` | Optional subject facts and typed facts supplied by the host |
| `http_command` | Canonical HTTP method, relative target, optional body; status/body response |
| `http_stream` | Stream open intent, pull/emission/finish intent and typed stream event |
| `capability` | Host-selected pure validation policy, not a wire authority |
| `limits`, `exceptions`, `validation`, `codec` | Bounds, typed failures, pure checks and Raw adapters |

`forge.api.core.types` owns the error payload and API status. The independent
`forge.net.http.method` value module owns the HTTP enum. Their existing numeric
values and P2 enum wire widths are unchanged. This new protocol does not claim
binary compatibility with any product-specific Compute ABI.

## Bounded Use

```cpp
import forge.api.wasm.codec;

namespace protocol = forge::api::wasm;

// Product DTO serialization stays in Raw. The name is part of the binding contract.
auto request = protocol::encode("catalog_query", query);
auto command = protocol::effect{"catalog.read", std::move(request), "catalog_result", 0};
auto allowed = protocol::capability{
   .binding = "catalog.read", .request_type = "catalog_query",
   .response_type = "catalog_result", .max_delay_ms = 100};
protocol::validate(command, allowed);

// Select allowed from trusted host configuration, never from the guest request.
protocol::validate(received_event, command, allowed);
auto result = protocol::decode<catalog_result>(
   std::get<protocol::payload>(received_event.value), "catalog_result");
```

The caller handles the error alternative before extracting a successful payload.
`retryable` is information, not permission to repeat an external side effect.
The protocol performs no I/O, timers, retries, scheduling or persistence.

`limits::wire` is the existing `raw::unpack_limits`, passed directly to exact
decoding. Additional limits bound names, payload bytes, packed continuation and
HTTP target. Decode rejects trailing bytes, malformed alternatives and allocation
violations using existing Raw errors. Protocol failures use `forge.api.wasm`
typed exceptions natively and the existing guest assertion policy in WASM.
Callers must also bound the enclosing action input before SDK dispatch.

Encoding calls the selected Raw serializer once and checks its resulting bytes.
It is not a memory sandbox for arbitrary native serializers: native callers must
bound their DTOs before encoding, and guest execution needs the executor's memory
limits. Existing Raw `pack_size` also materializes bytes; aggregate validation
uses it only after field/count checks. No allocation-free size claim is made.

## HTTP And Trust Boundaries

`effect.response_type` pins the **whole** successful result, not its nested body.
An HTTP capability separately pins each permitted status and body type/presence:
required, optional, absent, or explicit status-only. An encoded nullable value
is present and differs from an absent body. Status-only describes the decoded
envelope; the host adapter is responsible for discarding a bounded raw body.

Pure checks enforce bounded origin-form intent, method values, body shape and
the selected binding. They do not implement URL authorization or an HTTP client.
The host adapter must use existing HTTP/URL components to enforce base origin,
allowed methods/paths, content type, TLS, credentials, redirects and network
limits before executing any request. Guest values cannot grant those permissions.

`context` is serializable host-supplied **facts**, not an authenticated subject
capability. The host establishes identity and admission outside WASM and restores
current facts on every continuation. Client input and trusted context remain
separate action arguments. This library neither verifies credentials nor makes
deserialized context trustworthy.

## Incremental HTTP Stream Contract

`forge.api.wasm.http_stream` owns `http_stream_open`, `http_stream_read` and
`http_stream_event`. The neutral feature name is `http.stream.v1`, exposed as
`http_stream_feature`. These values use the existing effect/continuation model;
they introduce no HTTP engine or host-side stream session implementation.

```cpp
import forge.api.wasm.http_stream;
import forge.api.wasm.codec;

namespace protocol = forge::api::wasm;
auto open = protocol::http_stream_open{
   {forge::net::http::method::get, "/items", {}}, "item", "END"};
protocol::validate(open);
auto pull = protocol::http_stream_read{};
protocol::validate(pull);

// Explicit typed decoding follows the selected binding's whole response type.
auto item = protocol::decode<protocol::http_stream_event>(received_payload, "http_stream_event");
protocol::validate(item, open.event_type);
```

Open validation reuses the normal HTTP intent checks, validates the item type
name, and requires an end marker of 1–64 bytes with no CR/LF. Read validation
bounds an optional emission; an emission and `finish=true` may share one yield.
Event validation checks status 100–599 and an optional bounded item. The overload
with `expected_item_type` also pins that item's type. All three validate packed
aggregate size. `ended` does not imply an item presence rule; application state
decides which terminal combinations and statuses it accepts.

The host owns connections, framing, decoding, cancellation and transport bounds;
the guest owns item interpretation and normalized emissions. Admission, allowed
origins/methods, credentials, operation persistence and retry/recovery decisions
remain with the host application. Generic capability validation never infers a
stream handler from a payload type name: the selected adapter explicitly decodes
the stream DTO and invokes its typed validator.

## Packaging And Evidence

Native: `find_package(Forge CONFIG REQUIRED COMPONENTS api_wasm)`.
Guest: link `Forge::forge_api_wasm` through the installed Forge Contract SDK.
The dependency closure contains API/HTTP values and Raw, not their runtimes,
VM, Asio, authentication, DB, LLVM or Clang. `execution_wasm` is a separate host
choice, never a dependency of these protocol values.

Focused tests live in `tests/api_wasm`; the installed-package consumer checks
dependency isolation and real validation. `guest/tests/api_wasm` compares
literal native, executed guest and generated ABI bytes for every variant,
plus malformed inputs and a product-owned action/DTO example. See
[the donor and acceptance note](../../../docs/donors/wasm-shared-execution.md).
