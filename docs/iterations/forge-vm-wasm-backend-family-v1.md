# Forge VM WASM Backend Family v1

## Status

Historical backend split. The WAMR experiment extends this family with separate
`compiler` and SDK `execution` leaves; `optimizing` remains the optimized mode,
not a promise to retain the EOS OC implementation. See the approved
[current boundary map](forge-vm-wasm-wamr-v1.md#library-boundary-review).
The original rename and its evidence below remain historical.

Accepted direction. The clean-breaking interpreter rename is implemented;
the second WASM execution backend remains deferred to its own review block.

## Problem

The current library owns a concrete EOS VM execution implementation but uses
the generic identity:

```text
path:       libraries/vm/wasm
target:     forge_vm_wasm
component:  vm_wasm
modules:    forge.vm.wasm.*
namespace:  forge::vm::wasm
```

That identity consumes the family root and leaves no clean sibling for the
planned EOS VM OC optimizing backend. Adding the optimizing backend below the
current target would either produce asymmetric names or make
`forge_vm_wasm` an accidental aggregate with ambiguous runtime behavior.

The existing target also contains the donor EOS VM interpreter and its current
x86_64 non-OC JIT mode. Those are one existing backend lane for the purpose of
this family split. The name `optimizing` is reserved specifically for EOS VM OC.

## Accepted Identity

The current implementation moves to:

```text
path:       libraries/vm/wasm/interpret
target:     forge_vm_wasm_interpret
component:  vm_wasm_interpret
modules:    forge.vm.wasm.interpret.*
namespace:  forge::vm::wasm::interpret
```

The future EOS VM OC implementation uses:

```text
path:       libraries/vm/wasm/optimizing
target:     forge_vm_wasm_optimizing
component:  vm_wasm_optimizing
modules:    forge.vm.wasm.optimizing.*
namespace:  forge::vm::wasm::optimizing
donor:      EOS VM OC
```

`libraries/vm/wasm` becomes an empty family root. It does not provide a
`forge_vm_wasm` target, a `vm_wasm` package component, an aggregate
`forge.vm.wasm` module or backend-selection aliases.

## Current Backend Migration

Every current public module moves mechanically below `interpret`. Examples:

```text
forge.vm.wasm.backend       -> forge.vm.wasm.interpret.backend
forge.vm.wasm.allocator     -> forge.vm.wasm.interpret.allocator
forge.vm.wasm.host_function -> forge.vm.wasm.interpret.host_function
forge.vm.wasm.types         -> forge.vm.wasm.interpret.types
forge.vm.wasm.watchdog      -> forge.vm.wasm.interpret.watchdog
```

Every current public symbol moves from `forge::vm::wasm` to
`forge::vm::wasm::interpret`. Installed consumers link
`Forge::forge_vm_wasm_interpret` and request component
`vm_wasm_interpret`.

The rename does not change:

- WASM parsing, validation or execution behavior;
- host-function ABI;
- allocator, watchdog, signal or exception semantics;
- deterministic SoftFloat behavior;
- donor source mapping;
- supported architecture matrix;
- the existing interpreter/non-OC-JIT selection inside this backend lane.

Whether the existing non-OC x86_64 JIT should later become a third backend is
a separate decision. It must not be silently moved into `optimizing`, because
that identity is reserved for EOS VM OC.

## Consumer Selection

Products select a concrete backend target explicitly. A product-owned runtime
config may choose between installed backends, but Forge does not hide the
choice behind link order, architecture detection or an aggregate target.

Initial Spine migration is mechanical:

```text
forge_vm_wasm                    -> forge_vm_wasm_interpret
Forge::forge_vm_wasm             -> Forge::forge_vm_wasm_interpret
import forge.vm.wasm.*           -> import forge.vm.wasm.interpret.*
forge::vm::wasm::*               -> forge::vm::wasm::interpret::*
```

Spine execution behavior remains interpreter-lane behavior until Spine
explicitly adds and configures `forge_vm_wasm_optimizing`.

## Compatibility Policy

This is an approved pre-stable source and package break:

- no forwarding modules;
- no namespace aliases;
- no CMake target aliases;
- no compatibility package component;
- no deprecated aggregate target.

The release notes provide the mechanical migration table instead.

## Implementation Order

1. Move the current library to `libraries/vm/wasm/interpret`.
2. Rename target, component, modules, namespaces and test targets.
3. Update Forge package exports, package consumers and documentation.
4. Update all Forge tests and donor mapping gates without changing donor code.
5. Update Spine and other in-workspace consumers in coordinated branches.
6. Run interpreter, host-function, watchdog, SoftFloat, package relocation and
   architecture-specific regression suites.
7. Release the clean-breaking Forge version before merging dependent products.

The optimizing backend is a later implementation block. Its donor audit,
code-cache lifecycle, executable-memory policy, platform support and
interpreter parity tests are required before production use.

## Acceptance

- no production target or installed component named `forge_vm_wasm`/`vm_wasm`;
- no public module matching `forge.vm.wasm.<leaf>` outside the two backend
  namespaces;
- no public symbol directly in `forge::vm::wasm`;
- current valid interpreter corpus remains behavior-identical;
- package relocation works through `Forge::forge_vm_wasm_interpret`;
- donor source and generated spec-test mappings remain unchanged;
- static gates reject compatibility aliases and accidental aggregate modules;
- Spine compiles and passes execution parity after the mechanical migration.

## Non-goals

- implementing EOS VM OC in the rename block;
- creating a runtime backend factory in Forge;
- changing contract wire formats or host ABI;
- changing consensus-visible execution policy;
- retaining old source or package names.
