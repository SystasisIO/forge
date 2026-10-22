# Usage-Aware Wamrc CLI Qualification

This is a focused, test-only source selector. It neither modifies Forge's vendor
checkout nor substitutes its production source identity. The candidate is
unpublished; parent owns review, publication and the eventual pin update.

## Candidate

- Maintained fork: `artifacts/wamr-public-migration-20261016/wamr-fork` under the
  Storlane workspace, branch `forge-runtime-profile-v1`.
- Base: `66e6be6c626856758ada54ddac2fc81ae49f5648`.
- Only source delta: `wamr-compiler/main.c`, correctly typed allocator adapters.
- Main SHA-256: `02070b87c1a0a3f6f7dc1363ca615cc12b37beb4a4cdd7684eb634867a817138`.
- `git diff --binary --no-ext-diff HEAD -- wamr-compiler/main.c` SHA-256:
  `128c84f4bdd356193b806f082992d00f74075cc607f2d6e968b643593145d886`.
- Controlled whole-working-tree archive SHA-256:
  `33a4b9ecb79825662fff75556d0e3c6fe08f12cfb6be74609a81664733af25c4`.
- Shared profile SHA-256:
  `3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`.

The archive was taken while only main.c was dirty. A full recursive comparison
against pinned vendor source, excluding `.git` and the extraction marker, reports
only main.c different. No build-time patching occurs. This snapshot deliberately
precedes the separately owned blocktype-policy work.

## Reproduce

Run from the Forge experimental checkout. `ARCHIVE` is the preserved
`tests/vm_wamr/compiler/build-cli-66-usage1/candidate-source.tar.gz` absolute path;
`BUILD` is an owned build directory. An explicit controlled checkout can instead
be selected with `WAMR_CLI_SOURCE`, without archive arguments.

```sh
cmake -S tests/vm_wamr/compiler/cli -B "$BUILD" -G Ninja \
  -DWAMR_CLI_ARCHIVE="$ARCHIVE" \
  -DWAMR_CLI_ARCHIVE_SHA256=33a4b9ecb79825662fff75556d0e3c6fe08f12cfb6be74609a81664733af25c4 \
  -DWAMR_CLI_BASE_REVISION=66e6be6c626856758ada54ddac2fc81ae49f5648 \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm
cmake --build "$BUILD" --target qualification_cli -j2
ctest --test-dir "$BUILD" --output-on-failure -V
```

The fixture generator uses matching Clang and `wasm-ld` (LLD 22.1.8), then invokes
the actual wamrc executable, not the library compiler. Both CLI target and engine
source compilation consume the existing shared profile helper. The same recovery
test is registered in the existing compiler qualification project.

For ASan and indirect-call type checking, configure a separate owned build with
`-DCMAKE_C_FLAGS='-fsanitize=address,function -fno-omit-frame-pointer'` and the same
`CMAKE_CXX_FLAGS`. Run CTest with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

Optional `WAMR_CLI_BASELINE_MAIN` selects the original pinned main.c as a diagnostic
control. Build `wamrc_callback_baseline` explicitly. It has the same profile,
instrumentation and runtime/codegen archives as the candidate, but is never
installed or used as a successful qualification compiler.

## Results And Cause

Owned evidence directories are `tests/vm_wamr/compiler/build-cli-66-usage1` and
`tests/vm_wamr/compiler/build-cli-66-usage1-asan` under this Forge checkout.
Both `ctest.log` files pass 2/2 tests: serial recovery and effective feature flags.
Each recovery test has 16 successful compilations and 32 normal failures covering
invalid magic, malformed type section with valid WASM header, missing input and
unwritable output, alternating with success. AOT v6 magic and byte-for-byte
deterministic output are checked. Signals and timeouts do not count as rejection.
Every invocation uses its scoped work directory; temporary stack-usage files do
not land in the repository root. Repeated invocations are isolated CLI processes,
not a claim about same-process WAMR compiler/runtime composition.

The ASan control's `baseline-start.log` proves the first invalid write: reading
89 bytes into a one-byte malloc allocation in `bh_read_file_to_buffer`.
`baseline-valid.log` repeats this with 343 bytes. Original main.c registered raw
libc callbacks, while usage1 passes `(Alloc_For_Runtime=0, size)` to malloc and
additional arguments to realloc/free. Hence libc receives zero instead of size;
the buffer is already corrupted before module parsing or LLVM teardown. The
unmodified warm Release CLI also failed on the same valid 343-byte fixture
(exit 138, `baseline-valid.log`). The historical exit139 map-destructor report
is compatible with this prior corruption; no independent LLVM/libc++ failure was
reproduced after fixing callbacks. No toolchain, runtime or codegen patch was made.

Release `candidate-start.log` and `candidate-fixture.log` show actual successful
compilation of the parent's 89-byte start and 1760-byte standalone fixture.
`final-feature-comparison.log` verifies 49 explicit WAMR definitions across 80
candidate CLI/engine translation units, 81 existing library-compiler units and
40 runtime units. The sole permitted runtime difference is `WAMR_COMPILER=0`.
The small CTest profile comparison also checks CLI main, wasm_memory and AOT
emission against the library compiler when `WAMR_CLI_LIBRARY_COMMANDS` is given.

Release CLI SHA-256:
`d4a4f38ec4ca3a8fc97fc10a54f1fd983a3c69739c21979f312840dedeb7de08`.
ASan/function-sanitized CLI SHA-256:
`42f40ca87c942d12c1ae1a1cf388c8ea57034291fbc19204255044b3ef31f479`.
The usage1/user-data0 profile is executed. Other allocator-prefix combinations
are not claimed as newly runtime-qualified by this focused result.

Remaining integration gate: parent must review/publish the combined fork, update
the immutable pin, rebuild and run fresh standalone matrices. No stale binary
identity sidecar was refreshed. Stack/reservation qualification is separate.
