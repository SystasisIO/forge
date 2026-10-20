# Linux Unwind Diagnostic

The branch-scoped workflow uses unchanged Forge
`830b4cad6bf00dde0519cb7cd33d231a40bbeb43` and WAMR
`da5892640f9168df5a097d096e6a3924192d6f86` for its runtime baseline.
Only a push to `vm-wasm-linux-diagnostics-v1` affecting the diagnostic files
triggers it; no default-branch workflow or runtime setting is changed.
The script verifies the exact CI65 run39406169819, successful producer
job118572518399, artifact12266644506, both archive digests and recipe
`cbd0880dd7325bae781da9da1ae851d7f7c42aa36d11fb28506b1f86f4d48583`, then uses
the existing unpack, ABI and header verifiers. The probe itself never downloads,
extracts or changes a toolchain.

## Symbol-Link Comparison

This experiment uses the FINAL relocated LLVM22.1.8 compiler and runtimes.
It is not the historical producer Stage0 compiler or retired Stage A runtime
bytes, and is not an exact reproduction of that producer's capability check.
The workflow inherits the qualified consumer environment, including exported
`CXXFLAGS` and `LDFLAGS`; it does not override C flags, linker flags, build type,
linker executable or RPATH specifically for this probe. Inspect the recorded
effective values rather than assuming the historical producer's flags.

The following standalone configure matches the script's probe invocation.
Use the same qualified environment and a fresh output directory:

```sh
cmake --debug-trycompile \
  -S "$GITHUB_WORKSPACE/.github/probes/vm-wasm-linux-diagnostic" \
  -B "$DIAG_ROOT/unwind-probe" -G Ninja \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DFORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX="$LLVM_PREFIX"
```

The one diagnostic-specific input is `FORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX`.
DEFAULT and PINNED both use `CheckFunctionExists` C executable link checks for
`__register_frame`, `__deregister_frame`, `__unw_add_dynamic_fde`,
`_Unwind_Backtrace` and `_Unwind_GetIP`. Only `CMAKE_REQUIRED_LIBRARIES` differs:
empty versus exactly `<prefix>/lib/libunwind.so.1`, resolved inside that prefix.
Independent result variables prevent cross-mode cache reuse. Supplied results
and non-linking static-library checks are refused; no check executable runs and
no `HAVE_*` is forced.

`unwind-probe.txt` records both modes, raw/boolean results, library SHA and
compiler/flags/search inputs. Retain the configure argv/exit/log, cache,
`CMakeFiles/CMakeConfigureLog.yaml` and debug TryCompile C/link inputs.
Configure exit `0` means facts were collected, not that a symbol was found.
DEFAULT false / PINNED true supports a required-library explanation only for
this final-bundle comparison. It does not establish a producer fix: that still
needs the real upstream `CheckSymbolExists` check with its
[declaration header](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/cmake/unwind.h)
and original producer inputs. Preserve false results without preseeding or
export workarounds.

## Separate Binding Observer

The baseline runs the unfiltered ten-case profile test and canonical five-test
installed package without loader tracing. The later observer repeats the actual
profile registry argv/cwd and 120s timeout with `LD_DEBUG=bindings` and an absolute
`LD_DEBUG_OUTPUT`; it does not select `bindings,files`. It retains the real PID,
exit, image/provider hashes, readelf records and PID-suffixed raw traces.
The separate scrubbed installed-helper observer uses no arguments: expected
exit `2` proves loader entry only, not helper protocol or compilation acceptance.
These launches are performed by the script, not by the standalone CMake command.

No LD_LIBRARY_PATH, LD_PRELOAD, LD_AUDIT, LD_BIND_NOW, test filter, RPATH edit or
replacement binding program is used. ldd presence is not a symbol binding;
absent bindings remain NOT_OBSERVED. Newly rebuilt 830b executable hashes are
not original CI65 executable identities. Baseline failures remain red even if
later observation succeeds. This is diagnostic evidence, not a runtime repair
or full-suite qualification; the new sibling probe does not alter the existing
`llvm-cxx-abi` recipe inputs.
