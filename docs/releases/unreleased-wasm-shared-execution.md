# Unreleased: Shared WASM Execution And Strict HTTP JSON

Forge P1-P5 changes based on `5f6a9ecdd3613bbd953dafd693a5dddfeaeedeae`.
Fresh local integrated validation passed on 2026-10-15. This note records the
tested Forge scope; exact-source review and delivery are tracked in the PR.
Project version is unchanged; no release or downstream acceptance is claimed.
T1/T2 downstream migration and Gate are excluded.

## Added Components

| Package component | Target | Responsibility |
|---|---|---|
| `api_core_values` | `Forge::forge_api_core_values` | Owns existing `forge.api.core.types` without API runtime |
| `net_http_values` | `Forge::forge_net_http_values` | Owns `forge.net.http.method` and `is_idempotent` without HTTP runtime |
| `vm_wasm_execution` | `Forge::forge_vm_wasm_execution` | Preview synchronous bounded pure Contract SDK action executor |
| `api_wasm` | `Forge::forge_api_wasm` | Preview portable payload, continuation, effect, step/event, context and HTTP intent/result values with pure validation |

Existing API/HTTP runtime targets consume the value owners;
`forge.net.http.types` still re-exports the method module. The value components
and `api_wasm` compile their canonical sources independently for native and SDK
guest use, without native archive/BMI reuse. They do not pull in HTTP runtime,
VM, Asio or compiler tooling. The separate host executor uses the interpreter
and canonical SDK intrinsic registry, not the SDK test host or LLVM compiler.

## Shared Enum Bytes

`forge.raw.enum_wire_type` makes explicit `enum_wire_type<Enum>` specializations
with `shared = true` authoritative for native Raw, guest Raw and Abigen.
Canonical API enums retain signed 64-bit scalar encoding; HTTP methods retain
signed 32-bit encoding. Numeric values, C++ underlying types, native canonical
error bytes and reflection field order are unchanged. Non-opt-in defaults and
legacy specializations remain unchanged; shared scalars are range-checked.

Historical `std::array<Enum, N>` object bytes remain underlying-type bytes and
its generated ABI uses underlying integers, not the scalar enum wire alias.
Vectors retain count-prefixed scalar encoding. Abigen rejects shared-enum C
arrays rather than emitting an incompatible fixed-array ABI. Rebuild native
and guest consumers and regenerate ABI artifacts when adopting shared metadata;
do not infer wire width from `sizeof(Enum)`.

## Execution Ownership

One compiled instance admits one invocation; overlap fails with typed `busy`.
Every invocation gets fresh memory, globals and context, owned return bytes,
and one joined timeout/stop watchdog covering initialization, start and apply.
Independent cancellation requires independently compiled instances with exclusive
code allocators: `backend::share()` is rejected by invocation admission.
The caller owns scheduling on its compute lane, the stop token and object
lifetime; this leaf adds no scheduler, cache, networking or persistence.

Guest traps, timeout and cancellation permit fresh reuse. Host callback/native
failures poison the instance: discard it; later calls return typed `unusable`.
Code-page restoration failure also forbids reuse; failure to disable code pages
during interruption remains a process-termination boundary. Arbitrary native
callbacks cannot be forcibly preempted. This is neither instruction fuel nor
process isolation.

## Protocol Trust Boundaries

`api_wasm` performs bounded exact Raw decode and host-selected capability checks,
not effect execution or credential verification. Serializable context contains
facts, not authority; the host establishes identity/admission and reconstructs
current facts on continuation. Guests cannot select their own permissions.
The host adapter enforces origin/path/method, TLS, credentials, redirects and
network limits using existing HTTP/URL components.

`effect.response_type` names the whole successful result. HTTP status/body type
and presence are separate policy; encoded null is not absence. Status-only
requires bounded raw-body disposal by the host. Type-name matching alone does
not validate product payloads: use their pinned decoder. Native encoding
serializes once before checking output bounds, not inside an allocation sandbox;
callers bound native DTOs and guest execution needs VM memory limits. No binary
compatibility with a downstream product-specific ABI is claimed.

## Strict HTTP Migration

This explicitly approved input-compatibility break selects existing exact JSON
decoding for typed HTTP requests, success responses and error envelopes,
including streaming fixed arguments. Required bodies/members, integer bounds,
member types, unknown fields and indexed variant shape are enforced. Malformed
remote error bodies become a generic failure without fabricated remote typed
identity or raw-body text in its message.

Migrate producers to in-range numeric JSON integers, all required members, no
unknown members, and exactly `[integer_index, payload]` with the matching payload.
Regenerate OpenAPI and client models together: integer schemas now carry exact
8/16/32/64-bit bounds and variants use correlated complete-array `oneOf` branches.
Optional omission/null, absent explicit `body<T>`, and bodyless route/query-bound
controls remain supported. Standalone JSON defaults, XML, Raw, SDK ABI and
path/query/header/cookie text conversion are unchanged; no legacy HTTP mode is
added.

The [Python consumer](../../tests/api/openapi_python/README.md) validates original
JSON against the exported schema **before** untouched generated Pydantic models
with `strict=True`. Exact integer/Decimal parsing avoids float rounding and
rejects duplicate keys/non-finite constants. Stock Pydantic still accepts boolean
literal indexes; the schema-first consumer rejects them recursively. Qualification
pins generator 0.83.0, Pydantic 2.14.1 and jsonschema 4.26.0 on Python 3.12.11.

The recorded corpus has 110 inputs across 18 schemas, with four explicit numeric
lexical differences, not universal accepted-input equality:

| Original JSON | JSON Schema | Exact HTTP | Schema-first Python |
|---|---|---|---|
| `7.0` | Accept | Reject | Reject (`StrictInt`) |
| `7e0` | Accept | Accept | Reject (`StrictInt`) |
| `[0.0,7]` | Accept | Reject | Accept |
| `[1.0,"text"]` | Accept | Reject | Accept |

Use ordinary integer spellings for interoperable inputs. `x-forge-json-profile`
documents this boundary; it is not another validator. Arbitrary schema
renames/aliases, enum naming and custom codecs are not proven. The existing
root-int64 JSON writer limitation is characterized, not repaired; optional-member
omission versus Python null output is not byte-identical serialization.

## Integrated Validation

This section records validation before the WASM-family rename. Historical
evidence used the component name `execution_wasm`; it is not new validation of
`vm_wasm_execution` or WAMR. See the
[boundary migration note](unreleased-wasm-family-boundaries.md) for that work.

Fresh coordinator-owned checks on macOS arm64, LLVM 22.1.8/Ninja passed:

- Native `forge` exports and focused targets built; focused CTest **27/27** in
  **33.36 s**, including Python, structure and SDK policy gates; extended HTTP
  **2/2**, repeated cancellation **10/10** and repeated strict HTTP **3/3**.
- The interpreter-only CMake finding was fixed and scoped rereview was CLEAN:
  ON compile-command macro proof, build/test passed; OFF was restored and rebuilt.
- Installed native exports supported five separately configured consumers:
  `api_http`, `api_core_values`, `net_http_values`, `execution_wasm`, `api_wasm`;
  configure/build/run and final executable reruns passed.
- Fresh SDK build/install, relocation with the original path absent, both guest
  fixture rebuilds and byte-identical WASM/ABI/metadata/manifest comparisons passed.
  Full Abigen fixtures passed. Installed-native plus relocated-SDK wire parity
  **1/1** and API protocol parity **1/1** passed.

The [donor note](../donors/wasm-shared-execution.md) records evidence filenames.
`.cache/acceptance/wasm-shared-execution/final/` is coordinator-local evidence,
not a promise of shipped logs; earlier P2/P3/P4/P5 receipts remain historical.
Python remains the bounded 110-input/18-schema qualification above, not universal
equality. Full CI, Linux, x86 JIT, sanitizers and downstream/live acceptance are
**NOT_RUN**; SDK relocation is not global native-package relocation or hermetic
cross-platform proof. This documentation pass runs no builds or tests.
