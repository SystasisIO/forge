---
name: create-contract-library
description: Use when creating, extending, refactoring, or reviewing a contract library compiled independently for a native host and a WASM guest.
---

# Skill: Contract Library

## Prerequisite

Apply `skills/create-library/SKILL.md` first. Contract libraries use the same
`.cppm`, `.cpp`, `.hxx`, naming, pairing and include-path rules as every other
Forge library.

## Build Model

A product owns two independent CMake configurations:

- the native host project;
- the standalone guest project configured with `ForgeContractToolchain`.

Both projects add the same shared source directory with `add_subdirectory`.
The sources are compiled separately for the selected toolchain. Native
archives, BMI and PCM files are never reused by the guest.

The ordinary CMake target graph is the only build graph. Do not create,
serialize, fingerprint, install or reconstruct a second dependency graph.
In particular, do not traverse or interpret `LINK_LIBRARIES`,
`INTERFACE_LINK_LIBRARIES`, `LINK_ONLY` or CMake directory wrappers. Forge may
compare raw target-property snapshots only to reject post-declaration mutation;
that comparison must not become dependency discovery.

The guest declaration is complete. Do not mutate a target with
`target_sources`, `target_compile_options`, `target_compile_definitions` or
`target_include_directories` after `forge_add_contract_library()` or
`forge_add_contract()`. Forge rejects such mutation because CMake compilation
and Abigen must use the same semantic profile.
Do not set directory-wide compile options, definitions, include paths or
`CMAKE_CXX_FLAGS*` in the guest project. The Forge Contract toolchain owns one
C++ profile per standard CMake configuration and passes the selected profile to
Abigen.

## Declaration

```cmake
forge_add_contract_library(
   product_protocol
   ID product.chain.protocol
   MODULE_BASE_DIRS include
   MODULE_SOURCES
      include/product/chain/protocol/value.cppm
   SOURCES
      value.cpp
   PUBLIC_HEADERS
      include/product/chain/protocol/macros.hpp
   PUBLIC_LIBRARIES
      Forge::forge_chain_protocol
      Forge::forge_raw
   PRIVATE_LIBRARIES
      product_validation
)
```

The helper creates an ordinary static CMake library with a
`FILE_SET CXX_MODULES`, standard PUBLIC/PRIVATE dependencies and the guest
compile settings when called from a guest configuration.

- `ID` is the stable ABI metadata owner used by Abigen diagnostics.
- `MODULE_SOURCES` are public module interface units.
- `SOURCES` are ordinary implementation units.
- `PUBLIC_HEADERS` are rare macro-only public headers exported as a standard
  CMake `FILE_SET HEADERS`.
- `PUBLIC_LIBRARIES` are available to downstream consumers.
- `PRIVATE_LIBRARIES` are available only to the owning target.
- Dependencies must be guest-compatible Forge components or other contract
  libraries when the target is built for WASM.

Use only paths owned by the current library. Private textual includes are
discovered by the compiler and depfiles; they are not duplicated in a Forge
inventory.

## Protocol Surface

- Shared strong IDs, enums, action payloads, persisted values, index keys and
  pure checked calculations live in a dual-target contract library.
- A named action payload provides a public
  `static constexpr get_name()` returning
  `forge::chain::protocol::action_name`.
- Host code packs the payload with `forge::raw`; guest dispatch unpacks that
  same payload directly.
- Authorization, `multi_index`, `singleton`, mutations and dispatch are
  guest-only.
- Guest table aliases that belong in the ABI are declared in an imported
  guest module interface. Abigen does not reparse library implementation
  units; their compile definitions and options remain owned by CMake.
- RPC, signing orchestration, caches, retries and external JSON boundaries are
  host-only.

## Guest Project

```cmake
cmake_minimum_required(VERSION 3.31)
project(product_guest LANGUAGES CXX)

find_package(ForgeContract CONFIG REQUIRED)

add_subdirectory(../libraries/chain/protocol chain/protocol)
add_subdirectory(libraries/product)

forge_add_contract(
   product
   SOURCES entry.cpp
   LIBRARIES product_contract
)
```

`forge_add_contract()` is called only in the standalone guest project.
For an annotated contract class exported by a module, use
`forge_add_contract(product DISPATCH_MODULE product.contract LIBRARIES product_contract)`.
Its `.cppm` and paired implementation belong to the ordinary contract library;
the SDK generates the import-only dispatch source in the build directory.
Do not maintain an empty product `entry.cpp` or a forwarding contract class for
this case. `DISPATCH_MODULE` is exclusive with `SOURCES` and `DISPATCH_SOURCE`.
`forge_add_contract_project()` may be used by a native project as an optional
launcher; it configures the guest project but never reads its target graph.
When shared sources live above the guest directory, pass the product root to
the launcher with `SOURCE_ROOT` and to a direct guest configure with
`-DFORGE_CONTRACT_SOURCE_ROOT=<root>`. One common root must cover every shared
and guest-only contract library. Generated contract inputs may instead live
below the guest project's `CMAKE_BINARY_DIR`; arbitrary external source roots
remain unsupported.

## Packaging

The Contract SDK does not install or materialize downstream dual-target source
packages. Products consume shared source trees with `add_subdirectory`.
If a native library needs installation, use ordinary CMake
`install(TARGETS ... EXPORT ...)` and install its declared module/header file
sets; do not install BMI or PCM files.

## Required Validation

1. Build and import representative shared types on the native host.
2. Build the same physical sources in the standalone WASM guest project.
3. Prove identical `forge::raw` bytes for representative values.
4. Prove named-action ABI fields are direct and guest dispatch receives the
   same DTO.
5. Exercise persisted state through `Forge::forge_tooling_testing`.
6. Verify direct guest and optional launcher builds produce identical WASM,
   ABI and runtime manifest artifacts.
