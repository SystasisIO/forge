# Unreleased: ABI JSON scalar policy and total element budget

Compatible addition to `forge.chain.api.abi`: `abi_json_scalar_policy::exact`
can be selected explicitly by new `abi_json_to_bin` overloads. Both previous
overloads, their signatures/symbols and their default scalar conversions remain
available. Existing callers require no configuration migration; Raw bytes, ABI metadata,
binary decoding, package components and guest intrinsics are unchanged.

External JSON consumers can now reject string/float/bool conversion to integer,
integer narrowing overflow and numeric/string conversion to boolean before ABI
packing. The policy uses existing Schema exact scalar validation, including
signed/unsigned widths, 32-bit varints, canonical decimal 128-bit integers and
finite/range/precision checks for `float32`/`float64`. It follows the existing
ABI aliases, optional fields, arrays, records and variants and preserves typed
diagnostic paths and offsets.

Use the explicit trailing arguments `limits, abi_json_scalar_policy::exact`;
`compatible` selects the previous behavior. Protocol-specific scalar handlers
and positional record arrays retain their current semantics. A consumer must
choose exact mode at its own external-input boundary; this addition does not
silently strengthen existing applications.

`abi_serialization_limits` appends `max_total_container_elements`, defaulting to
the maximum `std::size_t` value. Consumers must rebuild because this public C++
value layout grows. The default keeps previous behavior; an explicit bound
limits aggregate dynamic/fixed array expansion across the same ABI traversal,
including empty records whose Raw bytes have zero width. The budget check runs
before array allocation and uses overflow-safe remaining-capacity arithmetic.
Metadata keeps its independent limits; ABI artifacts and Raw layouts are unchanged.

Focused tests cover the old compatible bytes, integer/varint bounds, malformed
boolean and numeric inputs, optional/alias/container paths, metadata overloads,
wide-integer canonical text, floating-point precision, zero-width fixed/nested
arrays, cumulative variable arrays and zero/exact budgets. Validation results
are recorded with the implementation review; this note does not announce a release.
