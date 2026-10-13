# Unreleased: package metadata, HTTP compatibility and S3

These additions belong to the next MINOR release train. Integration into `dev`
does not publish a release or change the current release tag.

## Contract SDK and ABI

- `forge_add_contract` accepts `ABI_ROOT_TYPES` and `ABI_METADATA` alongside
  declaration-local `COMPILE_DEFINITIONS`. Abigen can describe types carried in
  opaque payloads without adding artificial public actions.
- Optional `forge::abi-metadata/1.0` sidecars describe enum names and values.
  Existing Chain ABI artifacts and Raw layouts are unchanged. Consumers that
  trust a sidecar must authenticate its content with their package manifest;
  the ordinary contract manifest does not implicitly bind this extra file.
- Dynamic ABI codecs use optional enum metadata, support nested array/nullable
  composition, and provide opt-in exact scalar validation and aggregate element
  budgets. Existing compatible overloads retain their defaults. Ambiguous
  adjacent nullable layers are rejected because JSON null cannot distinguish
  their presence states.
- Prepared WASM validation checks a module without executing its start function.
  Product-specific import/export policies remain with the consumer.

See [exact scalar policy](unreleased-abi-exact-scalars.md) and
[static variant scalar conversion](unreleased-static-variant-scalars.md).
Boolean alternatives of `std::variant` converted through
`forge.variant.static_variant` now emit `[index, false]` or `[index, true]`, rather
than an integer payload. Forge's permissive reader still accepts the old numeric
boolean representation; external numeric-only JSON readers need to accept
boolean literals before adopting the corrected producer.
Portable native arithmetic types, including `long` on macOS, now construct the
matching signed, unsigned, boolean or floating variant value without ambiguous
conversion overloads. Existing wide-integer and long-double policies remain.

## HTTP and errors

- Guarded WebSocket routes admit or reject a request before the Upgrade handshake.
  Server shutdown cancels admission waits, and Upgrade preserves buffered frames
  for both plain and TLS connections.
- The existing HTTP server plugin accepts native route contributions. Its API
  minor version is 2.1; previous implementations inherit a typed unsupported
  response for the new method rather than becoming abstract.
- Typed HTTP bindings support an optional error renderer, including failures
  before method invocation. Without it, standard Forge error responses remain.
- DTOs containing query/header/cookie parameters no longer instantiate an
  implicit whole-body codec. Structured body fields use `body<T>`; plain DTO
  body decoding is retained.
- `FORGE_THROW_EXCEPTION_WITH_DATA` preserves typed operational error data with
  the normal exception category, source location and redacted diagnostic fields.

Rebuild native consumers: public option/value layouts have gained members,
including `abi_serialization_limits`, `route_options` and `publish_options`.
Do not mix old compiled consumers with new library binaries.

## Native plugin integration

P2P node and resolver lifecycle modules import only the types required by their
public declarations. Private resolver helpers live in their owning implementation
headers. This reduces the imported compiler state for applications composing many
plugins, without changing plugin APIs, configuration or runtime behavior.

## Optional S3 client

`FORGE_ENABLE_S3=ON` builds the Preview `net_s3` component using AWS SDK for C++
1.11.900. Its public asynchronous interface hides AWS types and uses a bounded
Forge compute pool for blocking SDK calls. It supports object/range operations,
streamed files, multipart transfers and temporary signed URLs. Uncertain
mutations retain typed reconciliation evidence and are not silently retried.
Cancellation preserves a snapshot of any known multipart session and cleanup
state. File uploads reject special files before opening them; callers must keep
the source path and contents stable until the operation finishes.

Validation uses the real SDK against a controlled loopback HTTP server. This
does not establish compatibility with a live S3 service, cryptographic request
authorization, deployed TLS or crash recovery. See the
[S3 contract and validation limits](../../libraries/net/s3/README.md#tests-and-limits).
