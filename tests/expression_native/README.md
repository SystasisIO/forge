# Native expression comparison

This is an opt-in research harness for choosing a bounded native expression
backend. It is not a Forge public expression API, a contract intrinsic, a
production sandbox or part of the default Forge build. No evaluator, parser or
intermediate representation is implemented here: the adapters invoke the pinned
upstream engines directly.

The current acceptance scope is macOS ARM64, Homebrew LLVM 22 and C++23. Linux,
guest execution, Spine integration and live provider requests are outside it.

## Inputs and candidates

- [sources.json](sources.json) pins CEL-C++, ExprTk and the Bazel bootstrap by
  revision and SHA-256. Third-party source downloads stay under root `vendor/`
  and are ignored by Git; normal Forge consumers acquire no new dependency.
- [corpus.json](corpus.json) contains 69 immutable fixtures, including nine timed
  scenarios. Integer values are decimal strings. Synthetic rates are not live
  prices. Source provenance and the restricted domain of each rewrite are explicit.
- CEL uses its upstream Pratt parser, type checker and runtime with checked
  `int64` arithmetic. Its parser/checker and comprehension options are configured
  explicitly; it is not a fully restricted financial-language implementation.
- ExprTk uses its upstream `double` parser/runtime with assignment/control/function
  restrictions and strength reduction disabled. It is not presented as exact
  money arithmetic. Separate probes exercise stock integer instantiation, lazy
  operators, mutation restrictions and compiler limits.

## Reproduce

Use Python 3.12+ for safe source extraction; the comparison and classifier use
only the Python standard library. Run these commands from the Forge root:

```sh
/opt/homebrew/bin/python3 tests/expression_native/prepare.py
/opt/homebrew/bin/python3 tests/expression_native/generate_cases.py \
   --output build/expression_native/generated/cases.inc

bash tests/expression_native/cel/build.sh

env -u CPATH -u CPLUS_INCLUDE_PATH -u C_INCLUDE_PATH cmake \
   -S tests/expression_native -B build/expression_native/exprtk -G Ninja \
   -DEXPRESSION_NATIVE_GENERATED_INCLUDE_DIR="$PWD/build/expression_native/generated" \
   -DEXPRESSION_NATIVE_EXPRTK_DIR="$PWD/vendor/exprtk" \
   -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
   -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
   -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
   -DCMAKE_BUILD_RPATH=/opt/homebrew/opt/llvm/lib/c++ \
   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/expression_native/exprtk -j 4

/opt/homebrew/bin/python3 tests/expression_native/test_compare.py
/opt/homebrew/bin/python3 tests/expression_native/compare.py \
   --cel build/expression_native/cel_native \
   --exprtk build/expression_native/exprtk/expression_native_exprtk \
   --output build/expression_native/acceptance \
   --iterations 10000 --warmup 1000 --samples 7

build/expression_native/exprtk/expression_native_exprtk_lazy_probe
```

The optional `expression_native_exprtk_int64_probe` target is excluded from the
default build. Build it separately and preserve the real compiler exit status
and diagnostics. A compiler rejection is research evidence, not a successful
integer backend or an excuse to replace the upstream numeric engine.

CEL's standalone Bazel package is staged into the task-owned upstream source
directory. Its original files are verified against the pinned archive before
building; no upstream evaluator/parser files are patched. Bazel repository data,
compiler intermediates and binaries stay in `build/expression_native/`. The
child build environments clear ambient header-search overrides. Both candidate
builds explicitly use `-O2 -g0 -fno-fast-math -ffp-contract=off`.

## Reading the evidence

`compare.py` writes a JSON report, Markdown tables and raw per-process outputs.
It refuses to overwrite evidence. The compiled fixture SHA-256 must match the
original corpus bytes, and the report records both executable SHA-256 values.
Unexpected exits, missing metrics, stale fixtures and malformed output are
`HARNESS_ERROR`; they are never credited as arithmetic-error handling.

Expected errors specify their phase. A parser error cannot prove correct runtime
overflow detection. `NaN`/infinity returned by ExprTk remain native values and
do not count as engine-reported errors. Integer results are compared exactly,
without a floating-point tolerance.

Compilation includes candidate preparation. Evaluation reuses a prepared program
and bound variable values; formatting happens outside the measured loop. CEL
uses fresh evaluation arenas to avoid accumulating allocations. The common
result/checksum cost remains in both measurements. Timings describe these
scenarios, not the speed of one abstract operator: expressions and numeric
models differ. Process peak RSS includes the runtime and libraries; it is not
per-expression allocation or proof of a hard memory cap.

The source byte guard and child-process timeout protect this fixed research
corpus. They do not prove bounded execution of arbitrary hostile expressions.
The capability cases at the CEL comprehension limit are untimed, use upstream
exclusive-counter semantics and are explicitly separate from donor arithmetic.
The all-ones CEL/ExprTk expressions in those probes are not interchangeable
aggregates for arbitrary lists.

Current findings and acceptance evidence are in the
[dated donor report](../../docs/donors/expression-native-comparison-2026-10-23.md).
No candidate is automatically promoted to a product dependency.
