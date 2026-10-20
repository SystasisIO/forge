# WASM Compiler Command

Status: Experimental.

`forge.tooling.wasm_compiler.command` owns the isolated worker's fixed launch
arguments, resource limits and existing optimizing stdio protocol. Code generation
belongs exclusively to `forge.vm.wasm.compiler.compiler`; LLVM is not a command
dependency. The executable delegates directly to `command::run`.

The existing `compiler_session` owns scheduling, admission, cancellation, child
reaping and cache publication. This adapter creates no scheduler, process owner,
cache or alternative serialization format. Parent and helper must agree on the
exact engine, capacity, artifact and protocol identities before compiling.

`command::run` owns the isolated helper process, including its resource limits
and stdio descriptors. A dedicated close-on-exec descriptor carries only Raw
protocol frames; ordinary stdout/stderr are redirected to `/dev/null` so
upstream diagnostics cannot corrupt replies or disclose compiler input.

Do not link the optional compiler into an execution process. Worker failure,
timeout or unsupported source is a typed failure, never an interpreter fallback.

## Installed Helper Dependencies

On Linux, the helper target preserves external link directories in its install
RUNPATH through CMake's target-local `INSTALL_RPATH_USE_LINK_PATH`. Required
LLVM, Boost and crypto libraries must still be installed at those external
locations; this does not make the helper a self-contained distribution or alter
the parent runtime's dependency boundary. No inherited loader environment is
required or forwarded by the compiler session.

Qualify the installed executable under the real scrubbed child environment and
the complete installed optimizing session tests. A successful build-tree helper
or in-process compiler fixture does not establish that the installed helper can
load its dependencies or complete the protocol.
