# Host Tooling

`libraries/tooling` is an empty family for host-side build and process services.
It deliberately has no `forge_tooling` target and no `forge.tooling` aggregate
module. Consumers select only the capability they need:

| Library | Package component | Purpose |
|---|---|---|
| `abi` | `tooling_abi` | Clang AST to chain ABI and dispatcher |
| `attributes` | `tooling_attributes` | Clang attribute registration |
| `validation` | `tooling_validation` | ABI and WebAssembly validation |
| `manifest` | `tooling_manifest` | Deterministic build manifests |
| `testing` | `tooling_testing` | Deterministic VM and ObjectDB contract test host |
| `wasm_compiler` | `tooling_wasm_compiler` | Isolated compiler command; compilation belongs to `vm/wasm/compiler` |

Build the contract libraries with `FORGE_ENABLE_TOOLING=ON`.
Build `wasm_compiler` separately with `FORGE_ENABLE_WASM_COMPILER=ON` and
`FORGE_ENABLE_VM_WASM_OPTIMIZING=ON`; it does not enable the contract tooling
family. Its LLVM dependency belongs to the selected VM compiler leaf.
Only `abi` and `attributes` among the contract tooling libraries require a
compatible Clang/LLVM package. Contract
guest code, the wasm32 sysroot and command-line programs live in `guest/` and
`tools/` respectively.

`testing` executes contract WASM against deterministic authorization and
ObjectDB state. It is an SDK test fixture, not a controller, consensus runtime,
or production blockchain host.
