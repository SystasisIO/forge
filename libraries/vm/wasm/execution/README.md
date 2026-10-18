# WASM Action Execution

Status: Preview. Target `forge_vm_wasm_execution`, package component
`vm_wasm_execution`, namespace `forge::vm::wasm::execution`.

This synchronous host leaf compiles and invokes pure Contract SDK actions using
`forge_vm_wasm_interpret`. It does not own a scheduler, cache, persistence,
networking, authorization, Chain state or effect execution. Native consumers
run it on their existing Forge Asio compute lane and pass its stop token.
It is not the SDK test host and does not depend on Clang or testhost code.

```cpp
import forge.vm.wasm.execution.compiled_module;

namespace wasm = forge::vm::wasm::execution;
wasm::compiled_module compiled{code_bytes};
auto result = compiled.invoke(
   {.receiver = receiver, .action = action, .data = request_bytes},
   {.budget = std::chrono::milliseconds{100}}, stop_token);
// result.data owns the return bytes after guest memory is destroyed.
```

`receiver` and `action` use canonical Chain/SDK name values. They select the
dispatcher and confer no authority. `data` is borrowed for the synchronous
call only. The second `apply` argument (first receiver) equals receiver.

## Admission And Ownership

Imports and `apply(i64,i64,i64)->void` are validated before start executes.
Only `env.action_data_size`, `read_action_data`, `set_action_return_value`,
`eosio_assert_message` and `prints_l` are installed. Names and signatures come
from the canonical Contract SDK registry; pointer arguments lower to wasm32
offsets and the interpreter validates host spans.

Every invocation has fresh globals, linear memory and execution context.
Start and apply share one wall-clock watchdog budget and cancellation source.
One compiled instance admits one invocation at a time: overlap returns typed
`busy`, not an unbounded wait. Independent parallel instances must be compiled
independently. `backend::share()` cannot isolate a code allocator from timeout
and is explicitly rejected by the low-level invocation scope.

The caller must keep the object alive and must not move or destroy it during
an invocation. After guest traps, timeout or cancellation, fresh state permits
reuse. A host callback failure, including a guest assertion or an output/console
limit exception, conservatively poisons the compiled instance: discard it;
subsequent invocation returns `unusable`. Original typed callback errors are
preserved. Native failures are never reported as successful guest traps.
`bytes()` estimates resident compiled artifact size for a caller-owned cache;
it is not a peak-memory measurement or cache policy.

An OS failure to restore code-page access is `host_failure` and poisons the
instance. Failure to disable code pages during interruption retains the VM's
fail-fast process termination: it must not silently leave an endless guest
running. Applications needing containment of native/OS failures need an
external process boundary; this leaf does not supply one.

## Default Limits

Compilation uses canonical VM options composition, not a second parser policy
type. Defaults: 4 MiB input module, 4 MiB code/data segments, 1 MiB mutable
globals/local bytes, 16,384 section/table/local-set elements, 64 imports,
1,024 nested structures/symbol bytes, 256 memory pages and call depth 256.
Lenient parser flags are rejected. The full options are in `limits.cpp`.

Invocation defaults: 100 ms, 2 MiB input and return data. Console capture is
disabled by default; explicit capture is bounded and returned to the caller,
never logged. Assertion messages and module bytes are not exposed in errors.
This watchdog is not instruction fuel, process isolation or a guarantee that
arbitrary native callbacks can be forcibly stopped. The profile installs only
bounded SDK adapters; broad host functions do not belong here.

## Validation

`test_forge_vm_wasm_execution` covers admission, bounds, owned output, reset,
start interruption and cancellation propagation. The lower-level VM invocation
suite proves overlapping independent instances survive neighboring cancellation
and timeout, and covers sharing rejection, callback failure and exclusive admission.
`test_forge_package_vm_wasm_execution_component` exercises installed consumption.
Downstream Turbine migration and product acceptance are separate gates.
