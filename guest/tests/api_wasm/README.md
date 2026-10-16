# Independent Shared Protocol Fixture

The protocol library owns product DTOs in `fixture`, imports `forge.api.wasm`,
and is built natively and for WASM from the same module. The guest actions are
ordinary SDK actions; no Turbine manifest, product source or dispatch table is
required. `access` is a test policy, not an authentication implementation.

Build `guest` with the installed SDK toolchain. Build `native` against Forge and
that SDK, setting `API_WASM_WASM`, `API_WASM_ABI` and `API_WASM_METADATA` to the
generated artifacts. Run `api_wasm_parity` through CTest. The runner uses the
production `execution_wasm` adapter, not a mock guest serializer.

When configuring `guest` directly, set
`-DFORGE_CONTRACT_SOURCE_ROOT=<forge>/guest/tests/api_wasm` so the sibling
`protocol` module is inside the contract source root.

Checks compare hardcoded bytes, native Raw, separately constructed guest values
and generated ABI conversion. They also exercise the validation implementation
linked from the SDK archive, rejected malformed inputs, typed payload decoding,
host-selected capabilities and arbitrary action arguments. Package isolation is
tested separately by `tests/package_api_wasm_component`.

The shared fixture also carries opaque `forge.codec.json.document` values.
Literal native/guest bytes remain standard Raw strings, generated metadata
marks only the canonical JSON alias, and the native ABI adapter restores JSON
objects. Guest tests enforce Raw byte bounds and reject truncated strings;
native codec/ABI tests own malformed JSON, depth and text-size validation.
Raw `unpack_limits.max_bytes` bounds the string payload allocation, excluding
its encoded length prefix. The fixture accepts eight payload bytes at a limit
of eight and rejects nine, independently on the native host and WASM guest.

## Executed Stream Scenario

The same standalone package imports `forge.api.wasm.http_stream` and owns only
its `stream_request`, `stream_item`, `stream_state` and `stream_summary` DTOs.
The real compiled WASM action opens a stream, consumes two incremental typed
host events through serialized continuations, emits each normalized item,
requests finish and completes after its finish acknowledgement. Every action
invocation uses a fresh production `execution_wasm::compiled_module`. The host
withholds the terminal event until both emissions have actually been observed.

Literal native bytes, independently constructed guest values and generated ABI
conversion cover all three canonical stream records. Executed negative cases
cover incomplete streams, malformed typed items, invalid status/framing,
inclusive/exceeded payload bounds and an item arriving instead of the requested
finish acknowledgement; a host error is propagated through the step protocol.

This is an independent protocol/execution test using incremental typed host
events. It does not open HTTP connections or prove transport timing; HTTP/SSE
transport has its own tests. No product package, gateway or product runtime
library participates in this fixture's build or execution graph.
