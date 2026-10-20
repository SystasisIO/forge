# Post-Fix Linux Unwind Diagnostic

The branch-scoped workflow uses CI66's actual unchanged merge-context checkout
`be680e122ed3f8aa2f304ad3c3f0fd9f153748c4`, tree
`212f0f35004ca2dba8088ea92610da2439c3c33e`, with WAMR
`da5892640f9168df5a097d096e6a3924192d6f86` for its runtime baseline.
API PR head is separately pinned to `72a9e5aeaba390389aa35bbdc0fe7fc02fc968ab`,
tree `18d2b49b4db9e0d715bf54d8489cb99f34b1e773`. The actual checkout's ordered
parents must be `81093bd80cdda1296776b9d5921707869f4c3ee0` and that PR head;
both the raw local Git commit object and trusted API commit metadata verify the
distinction. The local check hashes the complete commit object and reads its
tree/ordered parent headers, so checkout's shallow boundary cannot hide parents.
Per-phase raw objects and parsed receipts are retained. Error traps propagate
through functions and identify the failing phase/line without printing tokens.
This does not claim identical whole merge/head trees or qualify unrelated merge changes.
Only a push to `vm-wasm-linux-diagnostics-v1` affecting the diagnostic files
triggers it; no default-branch workflow or runtime setting is changed.
The script verifies successful CI66 run39461336186, successful producer
job118761279892 and artifact12294556432 (281297384 ZIP bytes), outer SHA256
`1ea39a735d44b41334f24205185f7f918dcd7fd89bf15b7d05bc17ef461c6858`, inner SHA256
`3e979ff1d59538a0c2c1bcd90261ba418fd3182f520e3d028b072f6e933b673d` and recipe
`f1d097b804574c48e35b0c6bec99321e416b0fda5e92582afe9efa9236060143`, then uses
the existing unpack, ABI and header verifiers. The probe itself never downloads,
extracts or changes a toolchain. All eleven recipe input hashes are checked
against this actual source checkout; no new cache key or verification policy is introduced.

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
this final-bundle comparison. DEFAULT dynamic-FDE may remain FALSE because
ordinary C implicit links select gcc/gcc_s, unlike the producer's explicit
Stage A required-library check. Neither mode is forced or required to be TRUE.
This comparison is not a repeat of the real upstream `CheckSymbolExists` check with its
[declaration header](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/cmake/unwind.h)
and original producer inputs. The separate reviewed CI66 producer evidence
records that native check; this physical diagnostic does not recreate Stage A.
Preserve false results without preseeding or export workarounds.

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
absent bindings remain NOT_OBSERVED. Newly rebuilt be680 executable hashes are
not original CI66 executable identities. Baseline failures remain red even if
later observation succeeds. This is diagnostic evidence, not a runtime repair
or full-suite qualification; the new sibling probe does not alter the existing
`llvm-cxx-abi` recipe inputs.

The existing full Forge/helper/profile build and normal dev install retain
build parallelism 2, serial CTests, the workflow's 120-minute infrastructure
limit and unchanged canonical runtime timeouts (profile 120s, package 900s).
This source candidate is pending review/publication/execution; previous CI65
diagnostic commits, failures and R15 evidence remain immutable history.
