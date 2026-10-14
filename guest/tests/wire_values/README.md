# Shared Enum Wire Parity

This fixture compiles `protocol/include/fixture/wire_values.cppm` independently
for native C++ and WASM. It consumes the installed Contract SDK through its
ordinary CMake package; no native archive or BMI is reused by the guest.

The native runner executes the real WASM actions with Forge's existing test
host and compares native Raw, guest Raw and the generated ABI codec against
literal bytes. Coverage includes canonical API status/error values, HTTP
methods, signed and unsigned boundaries, vectors, fixed arrays, optionals,
variants, enum metadata and unchanged non-opt-in defaults. `fresh` creates
the nested value in WASM independently of the incoming byte buffer.

## Run

First build/install a Forge native package with `chain_api`, `tooling_testing`,
`codec_json`, `codec_hex`, `raw`, `api_core_values` and `net_http_values`, and
install a matching Contract SDK. The SDK must include the current Abigen and
foundation archives. Set absolute paths below; use the supported native
compiler and, on macOS, the SDK sysroot as in the repository build instructions.

```sh
ROOT=/absolute/path/to/forge
BUILD=/absolute/path/to/generated/wire-values
SDK=/absolute/path/to/installed-contract-sdk
NATIVE=/absolute/path/to/installed-native-forge

cmake -S "$ROOT/guest/tests/wire_values/guest" -B "$BUILD/guest" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/lib/cmake/ForgeContract/ForgeContractToolchain.cmake" \
  -DForgeContract_DIR="$SDK/lib/cmake/ForgeContract" \
  -DFORGE_CONTRACT_SOURCE_ROOT="$ROOT/guest/tests/wire_values"
cmake --build "$BUILD/guest" -j4

cmake -S "$ROOT/guest/tests/wire_values/native" -B "$BUILD/native" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DForge_DIR="$NATIVE/lib/cmake/Forge" \
  -DForgeContract_DIR="$SDK/lib/cmake/ForgeContract" \
  -DWIRE_VALUES_WASM="$BUILD/guest/artifacts/wirevalues.wasm" \
  -DWIRE_VALUES_ABI="$BUILD/guest/artifacts/wirevalues.abi" \
  -DWIRE_VALUES_METADATA="$BUILD/guest/artifacts/wirevalues.abi.metadata.json"
cmake --build "$BUILD/native" -j4
ctest --test-dir "$BUILD/native" --output-on-failure --timeout 900
```

Use a fresh guest build after relocating the installed SDK and compare the
WASM, ABI and enum metadata artifacts byte-for-byte. `run_abigen_fixtures.py`
in the adjacent tooling fixtures separately checks imported metadata discovery,
strict generation, invalid contracts and unsupported C-array shapes.

## Compatibility Boundaries

- Native described enum defaults remain `int64`; ordinary portable enum
  defaults remain their C++ underlying type. Only explicit `shared = true`
  specializations opt into the common wire type.
- Raw's existing `std::array<Enum, N>` object representation is retained.
  Its ABI uses the underlying integer element, not the scalar enum alias.
  C arrays of shared enums have a different Raw count-prefix contract and
  are rejected by Abigen instead of generating an incompatible fixed array.
- The CDT-compatible action dispatcher accepts trailing action bytes. The
  `decode` action separately exercises guest `unpack_exact`; the runner tests
  both behaviors rather than changing dispatch to make a negative test pass.
- An ABI integer alias describes wire width and signedness, not the narrower
  C++ underlying domain. Native and guest decoders additionally reject wire
  values that cannot fit that underlying type. This is not a claim that the
  ABI format can express arbitrary C++ constraints.
- Test-host invocation proves guest serialization and generated ABI behavior,
  not the planned production action executor or downstream product migration.
