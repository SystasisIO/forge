# Installed native expression acceptance

This is a runtime gate for a genuine `cmake --install --component dev` package.
It is separate from the metadata-only package test. The custom target first
builds the normal `forge` aggregate, including all enabled install targets;
it does not build unrelated test executables or manufacture missing archives.

Configure the producer with `FORGE_ENABLE_EXPRESSION=ON` and optionally set
`FORGE_EXPRESSION_PACKAGE_TEST_ROOT` to a **nonexistent** external directory.
Then run:

```sh
cmake --build build/expression-memory --target test_forge_package_expression_component --parallel 4
```

The wrapper installs into `original`, verifies dependency notices against the
producer manifest, and renames that prefix to `relocated`.
Only the standalone consumer and toolchain-only initial cache are copied outside
the producer trees. macOS `sandbox-exec` denies reads from the original source,
build and install prefix, and denies execution of the actual producer Bazel and
executables named Bazel/Bazelisk. Explicit denied-access probes are saved before
consumer configuration. The consumer then configures, rebuilds public modules,
links and runs under the same sandbox using `Forge::forge_expression`.
The consumer obtains the executable path from the installed `ForgeCEL::worker`
target and constructs one explicit `expression::runtime`. The wrapper verifies
that this relocated worker is byte-identical to the real producer executable.
Public calls use that persistent child; the consumer does not link CEL directly.

The executable checks integer results above 2^53, a 10^30 intermediate, unsigned
range, all named rounding modes, lazy branches, typed error codes and concurrent
use of one immutable program. It also saves a portable checked artifact, loads it
in a different real worker, compares its canonical bytes and result, and rejects
forged result metadata even after recomputing its digest. The runtime memory snapshot must retain the explicit
64 MiB worker capacity and satisfy `used <= peak <= capacity` without allocation
failures for these ordinary calls. Actual allocation-failure enforcement is a
separate focused gate; this consumer does not substitute snapshot checks for it.
Linux acceptance remains separate.

`result.json`, `commands.jsonl`, the sandbox profile and install/configure/build/runtime/denial
logs remain in the external acceptance directory. Command records include exact arguments,
working directories, exit codes and elapsed times. Partial logs and an `INTERRUPTED`
result are retained if an external monitor stops the acceptance run. An explicit existing directory
is rejected; the wrapper never deletes or reuses it. Without an explicit path a
fresh temporary directory is created and retained. CTest runs the same wrapper;
its producer libraries must already be built, as for other package tests.
