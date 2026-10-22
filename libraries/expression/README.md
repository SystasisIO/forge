# Native expressions

`Forge::forge_expression` / package component `expression` provides an opt-in,
native **Preview** profile backed by pinned CEL-C++. Enable it with
`FORGE_ENABLE_EXPRESSION=ON`. V1 is qualified on macOS ARM64 only.
The default build does not discover or download CEL or Bazel.

```cpp
import forge.expression.program;
import forge.expression.exceptions;

const std::array variables = {
    forge::expression::variable{"tokens"},
    forge::expression::variable{"rate"},
};
forge::expression::program price(
    "math.mul_div(tokens, rate, 1000000, rounding.ceil)", variables);
auto result = price.evaluate(forge::mutable_variant_object()("tokens", 1200)("rate", 7));
// result.value is the exact int64 value 1. No floating-point conversion.
```

Public modules expose Forge values, declarations, limits and typed exceptions;
they do not expose CEL, Protobuf or Abseil. The private bridge borrows scalar
values, string views and container accessors. It does not create an owning copy
of the caller's input object. Each evaluation creates its own CEL activation and
arena; prepared state is immutable and may be used concurrently. The caller must
not modify an input object while it is being evaluated.

## Version 1 semantics

- Declared inputs: `int64`, `uint64`, `boolean`, bounded `string`, flat
  `list_int64` / `list_uint64`, and string-keyed `map_int64` / `map_uint64`.
  Literal lookup tables may use integer or string keys. Nested containers,
  dynamic types and heterogeneous values are rejected.
- Results: `int64`, `uint64` or `boolean`. Missing, extra or wrongly typed input
  is an error; values are not coerced between signed/unsigned integers.
- Allowed operations: checked CEL scalar arithmetic, scalar comparisons,
  boolean operators, conditional selection, bounded list/map lookup and size.
  All AST nodes and resolved overloads are validated, including unselected
  branches. CEL's own lazy conditional and logical error semantics apply.
- No `double`, byte strings, timestamps, durations, Protobuf objects, optional
  values, conversions, regex, concatenation, collection equality/membership,
  macros or comprehensions. There is no recursion or user-defined function API.
- Arithmetic domain errors retain CEL semantics. The outer Forge boundary
  reports a remaining error with `expression::exceptions::code::evaluation_failed`.
  Resource exhaustion aborts evaluation directly and cannot be pruned by CEL
  logical operators. Diagnostics omit source expressions and input contents.

Catch `forge::exceptions::base` and compare `error.code()` with the owning
category's `make_error_code(...)`. Computed backend errors use Forge's standard
`runtime_coded_exception`; a particular concrete exception subtype is not
guaranteed by this API.

`math.mul_div(a, b, divisor, mode)` has separate signed and unsigned overloads.
Its first three arguments must have the same type; `mode` is an `int64`.
The product is computed in a checked, sufficient 128-bit intermediate, divided
and rounded, then checked against the result's 64-bit range. This supports
`10^15 * 10^15 / 10^15` without making 128-bit values part of the CEL language.
Ordinary CEL `a * b / divisor` continues to check its ordinary 64-bit product.

| Mode | CEL constant | Rule |
| --- | --- | --- |
| 0 | `rounding.trunc` | Toward zero |
| 1 | `rounding.floor` | Toward negative infinity |
| 2 | `rounding.ceil` | Toward positive infinity |
| 3 | `rounding.half_even` | Nearest integer; ties select the even integer |

Zero divisors, invalid modes and out-of-range rounded results are errors.
Signed division accepts negative divisors; for example, `-5/2` gives
`-2, -3, -2, -2` in the four modes. No saturation or wraparound is performed.

## Resource limits

Defaults are also fixed V1 ceilings; callers may lower them.

| Limit | Ceiling | Enforcement |
| --- | ---: | --- |
| Source bytes | 8192 | Before parsing |
| AST nodes | 1024 | Exact admission of parsed and checked trees; defensive parser/checker limits |
| Depth | 48 | Parser recursion and full tree admission |
| Declared variables | 64 | Before declaration assembly |
| Elements per collection | 256 | Inputs and literals |
| Bytes per string | 1024 | Inputs, keys and literals |
| Total input bytes | 65536 | Names, keys and typed payloads before CEL construction |
| Evaluated AST events | 8192 | Upstream trace callback, per invocation |

The event callback runs **after** a corresponding CEL step. A successful call
may use exactly its allowance; an exhausted call may execute one additional
AST event before being stopped. This is not a CPU-instruction counter or a
wall-clock deadline. Primitive widths and collection/string limits separately
bound work inside allowed operations. Macros and growing collections are
excluded from this profile rather than implemented by a second evaluator.

These are structural limits and an execution-event budget. They are **not a
hard cap on all native heap allocations**: CEL parser/checker/planner and
runtime use allocations outside the caller's Protobuf arena. Arena block size
or process RSS does not prove such a cap. Memory failure injection and a full
allocator/process isolation design remain a separate qualification gate before
accepting hostile programs in a consensus runtime.

Prepared programs are currently process-local values. V1 does not accept
publisher-supplied `CheckedExpr`, serialize canonical programs, define monetary
units, publish tariffs, access a network/database, or add a Contract SDK
intrinsic. Profile versioning of persisted/on-chain artifacts remains a later
explicit contract; no C++ pointer is a portable program identifier.

## Dependencies and verification

CEL-C++ commit `9c562a306569e90c8dea6d3af71f4433aa386162` and Bazel `8.7.0`
are checksum-pinned in [sources.json](../../cmake/cel/sources.json).
The build retains the upstream dependency graph and uses its public static
archive export rule. CEL/Protobuf/Abseil headers remain private. A checksum
failure or an unhandled link dependency fails the build. Upstream source is
staged under the build directory; it is not patched or linked from a donor repo.

Tests cover the public Forge module and typed errors, Python Fraction arithmetic
oracles, rounding/overflow boundaries, source/AST/input/event limits, forbidden
types in unselected branches, ownership/lifetimes, repeated calls and concurrent
evaluations. The arithmetic fixture is generated by
[`expression_arithmetic_oracle.py`](../../tests/expression_arithmetic_oracle.py);
`--check` verifies its reproducibility. Native acceptance is distinct from
Contract SDK/WASM/WAMR, Spine, Linux and real-provider acceptance.
