# Unreleased: canonical WASM option snapshots

This is a maintainer-approved, narrowly scoped pre-stabilization source-layout
change in the WASM interpreter options record. It is not a patch-compatible
ABI change and must be included in the next appropriate release notes before
publication. No release version or package component is changed by this note.

`forge::vm::wasm::interpret::options::max_linear_memory_init` changes from
uint32 to uint64, matching the existing parser's uint64 limit and allowing an
initialization range ending exactly at 4 GiB. The existing `options` record
also gains control-depth, legacy-depth, name-section and checker-presence
fields; numeric values are initialized to the existing generic parser defaults.

Mechanical migration:

- Recompile every C++ consumer that uses `options`; do not pass records across
  binaries built against different layouts.
- Use `resolve_options(custom_profile)` from
  `forge.vm.wasm.interpret.backend` when converting a static/custom profile to
  the owned options value. The result preserves omitted checker traits,
  section-limit inheritance and legacy acceptance flags.
- Preserve all fields of the snapshot in any downstream cache identity.
  Do not reinterpret/copy `sizeof(options)` bytes as a wire or persisted format.
  A downstream custom binary encoding of this record needs its own explicit
  version transition; Forge had not defined such an encoding previously.
- Keep admission policy separate from the execution profile of already
  admitted code. Resolving an execution profile must not add current admission
  limits to historical modules.

`module::validate_instantiation() const` is additive. It reuses the same
deferred element error and initial data-bound checks used by existing backends,
does not resolve host imports, and does not execute a start function.

Generic `default_options` and donor `compatibility_options` keep their existing
acceptance behaviour. New regression cases compare source profiles and their
resolved values, including absent checkers, 64-bit limits, static local/stack
flags, legacy execution flags and deferred instantiation failures.
