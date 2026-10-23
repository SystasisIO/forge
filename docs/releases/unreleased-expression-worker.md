# Isolated native expression worker

The native `expression` component is Preview. Its source API now requires an
explicit `forge::expression::runtime`. The runtime owns a persistent native CEL
helper, its allocation pool, process lifetime and transport. This source change
is intended for the next minor release, not a patch release.

Migration:

```cpp
import forge.expression.program;

forge::expression::runtime executor({.helper = "/trusted/path/forge-expression-worker"});
forge::expression::program formula(executor, "quantity * rate", variables);
auto value = formula.evaluate(inputs);
```

Replace `program(source, variables, limits)` with
`program(runtime, source, variables, limits)`. Keep the runtime alive until its
prepared programs and outstanding calls have finished. An injected Forge Asio
runtime must also remain running through `shutdown_async()`; synchronous calls
and destruction of a live runtime require a host thread outside its IO executor.

Installed CMake consumers use `find_package(Forge COMPONENTS expression)` and
can obtain the trusted helper path from `ForgeCEL::worker`. Ship the installed
worker with the package. Consumers no longer link or build the CEL archive and
do not need Bazel. The helper is selected by explicit path, not by `PATH`.

Async preparation, evaluation, memory inspection and shutdown return
Boost.Asio awaitables. The convenience constructor and injected-runtime variant
both use the existing Forge Asio mechanisms. In-flight cancellation, a deadline
or an unexpected worker exit invalidates that runtime's prepared handles; the
caller explicitly creates a fresh runtime. Calls are not automatically replayed.

The bounded pool covers native expression allocations, including retained
programs and allocator metadata. It is not an RSS or virtual-address limit.
Native stack, code/images, OS bookkeeping and parent transport state have
separate bounds or platform overhead. See the owning
[README](../../libraries/expression/README.md) for exact scope and limits.

The internal worker protocol is a versioned process-local implementation
detail. It is not a persisted program format, Contract SDK intrinsic or network
API. Arithmetic semantics and declaration kinds are unchanged. No persisted
program migration exists because the earlier Preview never supported persisted
programs.
