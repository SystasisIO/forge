# Unreleased: static variant scalar conversion

`forge.variant.static_variant` now constructs arithmetic alternatives through
the existing dynamic Variant scalar constructors. This removes ambiguous
free-function overload resolution for signed/unsigned 64-bit integers and
`double`, including exact JSON validation of optional static variants inside
described records on macOS.

Boolean payloads now retain their boolean dynamic kind, matching typed JSON
encoding, instead of being promoted to an integer by the free overload set.
The `[index, payload]` envelope and alternative indices do not change.
Ordinary Variant decoding still accepts previously emitted numeric booleans;
exact JSON keeps its existing requirement for boolean JSON literals.
Non-arithmetic alternatives retain their owning namespace conversion and
custom scalar/record adapters. No Raw format or ABI metadata changes are made.

Regression tests cover exact scalar JSON and nested optional described
records, full 64-bit integer ranges, scalar dynamic kinds, legacy numeric
boolean decoding and an owning namespace custom conversion.
