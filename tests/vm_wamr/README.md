# WAMR Qualification Experiment

Standalone, opt-in test project. No production backend is changed. The
[migration requirements](../../docs/iterations/forge-vm-wasm-wamr-v1.md) remain
mandatory even if every test here passes.
The [initial result](../../docs/donors/forge-wamr-qualification-v1.md) was 13/17
passing with explicit numeric and cancellation failures. See the
[patched qualification](../../docs/donors/forge-wamr-patched-qualification-v1.md)
for the follow-up, remaining gates and exact local evidence.
The [initialization follow-up](../../docs/donors/forge-wamr-initialization-qualification-v1.md)
records the earlier patch-set identity, deferred start and compiler leaf.
The [binding follow-up](../../docs/donors/forge-wamr-bindings-qualification-v1.md)
records the previous source/profile identity, module-local host bridge and
partial metadata/memory-limit qualification.
The [instruction/depth follow-up](../../docs/donors/forge-wamr-limits-depth-qualification-v1.md)
records the new identity, instruction/frame metrics, logical call depth and
the still-failing public-capacity gate. Unit-suite success does not override it.

## Current Quad Checkpoint

Source-only checkpoint for pin `156dbd0b6edea3d64b7e21d305913943b60b5f91`:
classic/fast interpreter, each in Release and Debug with ASan/UBSan. These four
lanes have **NOT_RUN** status for this checkpoint. LLVM JIT is excluded.
The root `*_probe.cpp` files already construct private binding functions through
`engine_bindings::describe()` or use empty vectors; no aggregate-initializer
adaptation or assertion changes are needed for the explicit PMR constructor.

The private probe links the same bounded loader and explicit JIT-disabled
aspect as the production interpreter, including `engine_load_budget.cpp` and
`engine_module_jit.cpp`.
The current source preparation copies the clean pinned fork; the patch-set
description below belongs to the historical experiment.
Supply absolute paths to the separately qualified `wamrc` and library probe
from this same source/profile, including their `.forge-identity` sidecars.
Old compiler outputs are not substitutes: configure validates both identities.
Do not run these commands until the parent releases the build slot.

Run from the repository root in Bash, setting `FORGE_WAMRC` and
`FORGE_WAMR_COMPILER_PROBE` to that compiler pair first. The four build
directories must be fresh; the commands do not remove existing directories.

```bash
set -euo pipefail
: "${FORGE_WAMRC:?Set the qualified pinned wamrc absolute path}"
: "${FORGE_WAMR_COMPILER_PROBE:?Set the matching library probe absolute path}"
LLVM_PREFIX=/opt/homebrew/opt/llvm
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
WASM_LD=/opt/homebrew/opt/lld/bin/wasm-ld

quad_lane() {
  local lane="$1" fast="$2" mode="$3" flags="$4"
  local dir="build/wamr-quad-156dbd0-${lane}"
  if test -e "$dir"; then
    printf 'Refusing non-fresh build directory: %s\n' "$dir" >&2
    return 1
  fi
  cmake -S tests/vm_wamr -B "$dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$mode" \
    -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
    -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
    -DCMAKE_ASM_COMPILER="$LLVM_PREFIX/bin/clang" \
    -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags" \
    -DFORGE_GUEST_CLANG="$LLVM_PREFIX/bin/clang" \
    -DFORGE_GUEST_WASM_LD="$WASM_LD" \
    -DFORGE_WAMRC="$FORGE_WAMRC" \
    -DFORGE_WAMR_COMPILER_PROBE="$FORGE_WAMR_COMPILER_PROBE" \
    -DFORGE_ENABLE_VM_WASM_LLVM_JIT=OFF \
    -DFORGE_WAMR_FAST_INTERP="$fast"
  cmake --build "$dir" -j2
  ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --test-dir "$dir" --output-on-failure --output-junit results.xml -j1
}

quad_lane classic OFF Release ""
quad_lane fast ON Release ""
quad_lane classic-asan OFF Debug "-fsanitize=address,undefined -fno-omit-frame-pointer"
quad_lane fast-asan ON Debug "-fsanitize=address,undefined -fno-omit-frame-pointer"
```

Keep all four results separate. Passing these private probes alone is not
public API, compiler, JIT or product acceptance.

## Historical Build

The following commands and patch-set notes record the earlier experiment;
use the current checkpoint above for fresh qualification.

Initialize only the pinned dependency with `git submodule update --init
vendor/wamr`. Use a Clang/LLVM installation with the wasm32 backend and `wasm-ld`.
The commands below run from the repository root. Set `LLVM_PREFIX` to your
toolchain installation. On macOS, supply the selected SDK through
`CMAKE_OSX_SYSROOT` when needed.
When lld is packaged separately, also set
`-DFORGE_GUEST_WASM_LD` to the matching `wasm-ld` executable explicitly.

```sh
cmake -S tests/vm_wamr/compiler -B build/wamr-compiler-library -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm"
cmake --build build/wamr-compiler-library -j 4

cmake -S tests/vm_wamr -B build/wamr-probe-initialization -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DFORGE_GUEST_CLANG="$LLVM_PREFIX/bin/clang" \
  -DFORGE_WAMRC="$PWD/build/wamr-compiler-library/compiler/vendor/wamrc" \
  -DFORGE_WAMR_COMPILER_PROBE="$PWD/build/wamr-compiler-library/wamr_compiler_probe"
cmake --build build/wamr-probe-initialization -j 4
ctest --test-dir build/wamr-probe-initialization --output-on-failure --output-junit results.xml -j 4
```

Use another build directory and `-DFORGE_WAMR_FAST_INTERP=ON` to qualify the
fast interpreter separately. Do not merge its results with the classic
interpreter without naming the profile.

Both projects prepare a build-local copy of the clean upstream pin with the
[numeric/cancellation patch](../../cmake/patches/wamr-deterministic-profile.patch)
[deferred-initialization patch](../../cmake/patches/wamr-deferred-initialization.patch)
the [native-call alignment patch](../../cmake/patches/wamr-native-call-alignment.patch)
and [auxiliary-stack boundary patch](../../cmake/patches/wamr-aux-stack-boundary.patch).
The shared [build profile](../../cmake/ForgeWamrProfile.cmake) is included by the
compiler/runtime and hashed into their identity. Memory shrinking is disabled:
guest ranges remain accessible through imports even without memory.grow/size.
They never modify the submodule. Compiler source identity and binary hash must
match the generated `.forge-identity` sidecar for both `wamrc` and the library
probe; an unpatched or changed compiler is rejected.
This prevents accidental local mismatches, not hostile compiler substitution.
Prepared source directories are build-owned; do not edit them.

For sanitizer qualification, use a fresh build directory with C/C++ flags
`-fsanitize=address,undefined -fno-omit-frame-pointer` and executable linker
flags `-fsanitize=address,undefined`. Tests set sanitizer halt-on-error, including
the subprocesses used for comparisons. A sanitizer warning is not a passing run.
All builds use WAMR's assembly native-call bridge (`WAMR_BUILD_QUICK_AOT_ENTRY=0`),
not quick C entries into generated code that lacks Clang function-sanitizer
metadata. This is the same runtime profile in Release and sanitizer runs;
neither sanitizer checks nor guest memory/stack/cancellation checks are disabled.

## Tests And Boundaries

- Integer values and a test-only host import.
- Native f32/f64 arguments/results, mixed i32/f64 cell offsets and FP argument
  register overflow into the platform stack-argument area.
- Initial memory, successful growth and declared-maximum failure.
- Guest traps and overflowing host pointer ranges. Error text is diagnostic,
  not a typed classification proof.
- Separate and fresh instances of one module.
- Termination of an actually entered infinite guest loop; concurrent active
  A/B cancellation isolation, rejected overlapping calls and reuse after cancel.
- Cancellation before initialization (no guest effects), during a real `start`
  loop, and between start and an export using the same stop token. Start trap
  cleanup, FP restoration, healthy instantiation after failure and no replay of
  initialization on one instance. Blocking host calls and complete public
  deadline/admission scope remain open gates.
- Seven targeted raw-bit samples plus 3165 corpus records (f32 and f64), compared
  across interpreter/AOT and all four host rounding modes. Arithmetic NaNs are
  canonicalized; abs/neg/copysign preserve payload bits. This is not the complete
  numeric corpus or cross-architecture determinism.
- Host callback poisoning, nested guest calls, FP environment restoration after
  normal return/trap/cancel and host flush-to-zero perturbation.
- Caller-owned bytes remain unchanged; instances retain code ownership;
  wrong argument/result cell buffers are rejected before entering WAMR.
- Compiler identity match and rejection of wrong profile, changed binary and
  missing identity metadata.
- The same runtime tests execute AOT generated by upstream `wamrc` and by the
  private `vm/wasm/compiler` helper. The helper additionally checks unchanged
  caller bytes, malformed input, input/output caps and repeat-compilation bytes.
  Byte caps are not a peak-memory/time bound for compilation.
- Module-local exact import binding without global native registrations;
  unknown imports, underscore aliases, NUL names and wrong signatures fail.
  The canonical SDK registry supplies all signatures; five callbacks and a
  synthetic SDK echo fixture test the bridge, not real SDK/product acceptance.
- Typed host failures never unwind through C/generated frames, preserve payload
  and take precedence over racing cancellation. FP state is restored after
  host failure. Incorrect callback argument/result types and missing results fail.
- Checked guest ranges/strings, overflow and end-of-memory boundaries; nested
  modules with different callbacks for identical names; concurrent contexts;
  same-instance overlap rejection and cooperative host cancellation.
- Caller page caps with a larger or absent module maximum; initial memory above
  the cap is rejected before instantiation. A non-growing fixture covers the
  entire declared memory tail. Logical pages are not WAMR's coalesced pages.
- Fourteen metadata limits plus mutable-global export policy, using the actual
  public options module. Threshold/one-less, zero and fallback rules are tested.
  Eight additional instruction/frame metrics cover body bytes, local sets,
  logical operands, normal/legacy control depth, branch tables and offsets.
  Early loader-allocation admission and the wider public ranges remain open.
- Exact direct/indirect/imported/exported-import depth boundaries in both modes,
  start admission and restoration after trap, host failure and active cancellation.
- `wamr_capacity_probe [output.wasm]` is a separate promotion gate. It must return
  zero before migration. Its current nonzero result is not inverted into a passing
  CTest: the same source built as `wamr_capacity_baseline` under the root project
  executes successfully through the unchanged public Contract SDK facade.

The probe now exercises private implementation components under the existing
`libraries/vm/wasm/interpret` leaf. They are not installed public APIs and are
not yet selected by `backend` or `compiled_module`. Probe-specific file loading
and host functions stay test-local. `std::jthread` is used only in bounded tests; no runtime
scheduler or ad-hoc production worker is introduced. CTest kills the test
process after 15 seconds if termination does not work. Do not run arbitrary
untrusted AOT artifacts: these fixtures are compiled locally by the pinned
compiler, and production provenance checks are not implemented here.
Private instance reuse tests do not establish public `backend::unusable` policy:
the public host-failure poison/discard wrapper remains to be implemented.

## Capture The Existing Baseline

Use `capture_baseline.py SOURCE_WORKTREE MANIFEST --archive ARCHIVE.zip` with
the current OC worktree. The script refuses to overwrite files. Use the same
arguments with `--check` (without `--archive`) to detect drift. Source contents
are not modified, staged or committed. The tracked inventory uses relative
file names; the optional archive belongs in the ignored local build directory.

Missing source directories fail capture; missing AOT compiler fails configure.
Neither condition is a successful or skipped acceptance gate.
