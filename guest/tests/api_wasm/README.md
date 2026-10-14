# Independent Shared Protocol Fixture

The protocol library owns product DTOs in `fixture`, imports `forge.api.wasm`,
and is built natively and for WASM from the same module. The guest actions are
ordinary SDK actions; no Turbine manifest, product source or dispatch table is
required. `access` is a test policy, not an authentication implementation.

Build `guest` with the installed SDK toolchain. Build `native` against Forge and
that SDK, setting `API_WASM_WASM`, `API_WASM_ABI` and `API_WASM_METADATA` to the
generated artifacts. Run `api_wasm_parity` through CTest. The runner uses the
production `execution_wasm` adapter, not a mock guest serializer.

Checks compare hardcoded bytes, native Raw, separately constructed guest values
and generated ABI conversion. They also exercise the validation implementation
linked from the SDK archive, rejected malformed inputs, typed payload decoding,
host-selected capabilities and arbitrary action arguments. Package isolation is
tested separately by `tests/package_api_wasm_component`.
