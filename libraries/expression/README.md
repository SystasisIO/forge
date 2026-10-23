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
forge::expression::runtime executor({.helper = "/path/to/bin/forge-expression-worker"});
forge::expression::program price(executor,
    "math.mul_div(tokens, rate, 1000000, rounding.ceil)", variables);
auto result = price.evaluate(forge::mutable_variant_object()("tokens", 1200)("rate", 7));
// result.value is the exact int64 value 1. No floating-point conversion.
```

Public modules expose Forge values, declarations, limits and typed exceptions;
they do not expose CEL, Protobuf or Abseil. The native CEL parser, checker,
planner and evaluator live exclusively in `forge-expression-worker`. An explicit
`runtime` owns one persistent helper and at most 64 prepared programs. Preparing
or evaluating a complete expression is one IPC exchange; arithmetic operators do
not cross the process boundary individually.

The parent packs the caller's borrowed input object directly through Forge Raw
into a bounded frame. The child decodes once and lends scalar/string/container
views to the existing CEL bridge. No second owning activation DTO is built.
Concurrent calls on one runtime serialize; separate runtimes provide explicit
parallelism and separate memory caps. Keep the runtime alive until its programs
and active calls are finished. Inputs passed to synchronous evaluation must not
be mutated concurrently; asynchronous calls take owning values.

`program::prepare_async`, `program::evaluate_async`, `runtime::memory_async`
and `runtime::shutdown_async` use Boost.Asio awaitables. Host-side synchronous
wrappers remain available. The runtime uses Forge Asio and Boost.Process for
transport, admission, deadlines and child retirement; it does not own an
alternative scheduler or reaper. An application may inject its existing Forge
Asio runtime. The convenience constructor owns the same Forge runtime type.

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

The helper additionally owns a fixed native allocation pool, up to 64 MiB.
`runtime::options::heap_bytes` selects its capacity. This pool covers the CEL
parser, AST, checker, planner, retained programs, activations, STL/Abseil/Protobuf
allocations and their allocator metadata together. A ready-made
Boost.Interprocess external-buffer allocator provides allocation and coalescing;
there is no system-heap fallback or new Forge heap algorithm. Static private
C++ runtime linkage and the build-time import audit keep these allocation
functions out of the parent process. Abseil's anonymous mapping requests use
the same bounded pool. `runtime::memory()` reports capacity, live storage, peak
and failed allocation count.

This is an allocation-pool limit, **not a process RSS or virtual-address limit**.
Native code/images, the OS loader, OS bookkeeping and parent transport state are
outside that pool. The helper separately limits its native stack to 8 MiB and
disables core dumps before private C++ initialization. Fixed runtime image state,
such as emergency exception storage and initial exit-callback slots, is also
separate from the pool. Its maximum pool reserves 64 MiB of address space; lowering
the active pool does not change that reservation. A server must budget the
selected pools, stacks, bounded parent state and platform overhead together.
Neither a Protobuf arena nor a sampled RSS threshold substitutes for this limit.

Some upstream allocation failures terminate a process rather than throw an
exception. A separate, nonblocking fault descriptor records pool exhaustion
before refusal; the parent can distinguish a worker that exhausted its pool
from an unexpected crash. A dispatched exchange failure retires and reaps the
worker, and its prepared handles become terminal. A retirement-notification
scheduling failure stops admission; the owner's mandatory shutdown barrier
performs physical cleanup. No request is silently re-executed. A fresh
runtime is the explicit recovery boundary. Deadline, cancellation, admission
and worker availability have separate Forge exception codes.

Prepared programs are currently process-local values. V1 does not accept
publisher-supplied `CheckedExpr`, serialize canonical programs, define monetary
units, publish tariffs, access a network/database, or add a Contract SDK
intrinsic. Profile versioning of persisted/on-chain artifacts remains a later
explicit contract; no C++ pointer is a portable program identifier.

## Dependencies and verification

CEL-C++ commit `9c562a306569e90c8dea6d3af71f4433aa386162` and Bazel `8.7.0`
are checksum-pinned in [sources.json](../../cmake/cel/sources.json).
The build retains the upstream dependency graph and uses its public static
archive export rule. Only the worker links that archive. CEL/Protobuf/Abseil
headers remain private. The installed component supplies the worker executable
and provenance; consumers do not rebuild CEL or run Bazel. A checksum
failure or an unhandled link dependency fails the build. Upstream source is
staged under the build directory; it is not patched or linked from a donor repo.

Tests cover the public Forge module and typed errors, Python Fraction arithmetic
oracles, rounding/overflow boundaries, source/AST/input/event limits, forbidden
types in unselected branches, ownership/lifetimes, repeated calls and concurrent
evaluations. The arithmetic fixture is generated by
[`expression_arithmetic_oracle.py`](../../tests/expression_arithmetic_oracle.py);
`--check` verifies its reproducibility. Native acceptance is distinct from
Contract SDK/WASM/WAMR, Spine, Linux and real-provider acceptance.

## Migration from the earlier Preview

Replace `program(source, declarations, limits)` with
`program(runtime, source, declarations, limits)`. Construct the runtime with an
explicit trusted worker path. Installed CMake consumers can obtain that path
from the `IMPORTED_LOCATION` of `ForgeCEL::worker`; the target follows the moved
installation prefix. Worker installation requires prefix-relative `LIBDIR` and
`BINDIR`; absolute or escaping destinations are rejected during configuration.
No executable is selected from the current directory or
an ambient PATH. The parent no longer links the CEL archive or its private
allocator. Prepared programs cannot be moved between runtimes or persisted.

The earlier native-profile evidence remains historical. Current isolated-memory
and installed-package gates are recorded in the
[follow-up validation report](../../docs/donors/expression-worker-memory-2026-10-23.md);
these are distinct from Linux, SDK/WASM/WAMR, Spine and provider acceptance.
