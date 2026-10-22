# forge_vm_wasm_interpret

`forge_vm_wasm_interpret` is Forge's native WebAssembly virtual machine. On this
experimental branch it uses a pinned WAMR fork behind Forge's C++23 module APIs:
validation, interpretation, optional LLVM JIT, host-function bindings, guarded
memory and bounded invocation. Donor-derived API/conversion code and its license
notices remain; replacing the execution engine does not erase that provenance.

This is a low-level VM library. It executes architecture-neutral WASM bytes;
the consuming product owns contract selection, code caching, scheduling,
state access, resource billing, and the host functions exposed to a guest.

## WAMR Experiment

On this experimental branch, the public `interpreter` and `null_backend`
specializations use the shared WAMR runtime owned by this target. Explicit `jit`
selection uses WAMR LLVM JIT when `FORGE_ENABLE_VM_WASM_LLVM_JIT=ON`; it never
falls back to interpretation or the old engine. The source build supports
x86_64 and ARM64, but each release/platform still requires its own qualification.
`jit_profile` selects instruction-level profiling through the same optional LLVM
runtime. It retains an immutable native-PC to original WASM-instruction map;
ordinary `jit` does not allocate this map. The new implementation is pending
runtime qualification, not a production acceptance claim.

`forge.vm.wasm.interpret.prepared_module` owns the generic prepared module,
module-local `host_binding`, and nested execution `context` (also named by the
`runtime_context` alias). The same context executes source WASM and trusted AOT
through private `engine_*` mechanics. It exposes neither the SDK registry nor
WAMR/LLVM types. Ordinary interpreter/AOT configurations do not link LLVM;
the explicitly enabled JIT runtime has a private LLVM dependency.
Source-plus-AOT preparation requires explicit trusted-compiler origin; structural
matching alone does not establish artifact provenance. Upstream stack-size and
structural-memory observations do not constitute exact resident allocation quotas.

Validation-only preparation uses WAMR `no_resolve` and projects import names and
function types from validated upstream metadata. It cannot instantiate guest
code. The interpreter retains the templated host registry and converters;
their donor-derived algorithms and corresponding license notices remain.

The usage-aware dispatcher now commits and grows the supplied `wasm_allocator`
mapping directly. Active execution borrows that allocator exclusively; an idle
instance is retired before reset or sequential handoff to another context.
Nested and parallel dispatch restore the correct owner. Local allocation or
protection failure is operational and prevents a subsequent host effect, while
module/caller growth-limit failure retains WASM's canonical `-1` result. The
private system provider zeroes initial storage and newly grown tails; public
contexts do not fall back to that provider.

The bounded profile currently admits at most 65,535 logical 64 KiB pages
(4 GiB minus 64 KiB), not 65,536. Initial declarations and explicit caller caps
above that representation bound reject before instance creation; the historical
`UINT32_MAX` caller sentinel means the common cap applies. A missing declared
maximum does not itself reject. Prospective native instance metadata must fit
`INT32_MAX` bytes, calculated from upstream layouts rather than an arbitrary
table-element cap. Boundary admission tests do not allocate multi-GiB memory.

Migration is not production-accepted: complete regression, profiling qualification,
cross-platform and installed/downstream gates remain open. Preparation uses an
enforced conservative reservation followed by actual retained-byte accounting,
not an exact prospective size. The three malformed-WASM permissions
`allow_code_after_function_end`, `allow_invalid_empty_local_set`, and
`allow_u32_limits_flags` no longer permit invalid bytecode in WAMR. Their source
fields remain usable, including `compatibility_options`; valid modules are not
rejected merely because those flags are true. This explicit maintainer-approved
change does not retire `allow_zero_blocktype`: the shared loader policy can
normalize a literal zero block type to void in its owned input copy, without
changing caller bytes or accepting arbitrary type-index encodings.

The consumer regression retains cached A-reset-B-reset-A with persistent
contexts and one allocator. Idle retirement is implemented; reset during active
guest execution, host callbacks or argument conversion is rejected instead of
mutating live memory. Fresh tests on the integration base remain mandatory.
The absolute-deadline `timed_run(deadline, interrupt_options, work)` API is now
present on the WAMR backend and covered by consumer-shaped compile/runtime tests;
this does not establish complete downstream or cross-platform compatibility.

The [compatibility checkpoint](../../../../tests/vm_wasm/WAMR_COMPATIBILITY.md)
separates current results from historical evidence. Do not include private engine
headers from products, load unauthenticated native artifacts, or claim EOS-derived
source removal.

## Package Integration

An explicitly LLVM-JIT-enabled distribution uses LLVM's CMake package, whose
dependency probes require both C and CXX languages in the consuming project.
Pass the matching `LLVM_DIR` and toolchain explicitly. This does not add LLVM
to the ordinary interpreter/AOT-only distribution or change the C++ runtime API.

```cmake
cmake_minimum_required(VERSION 3.31)
project(example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS vm_wasm_interpret)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE Forge::forge_vm_wasm_interpret)
```

The package exports:

- target `forge_vm_wasm_interpret` in a source build;
- target `Forge::forge_vm_wasm_interpret` in an installed package;
- component `vm_wasm_interpret`;
- namespace `forge::vm::wasm::interpret`.

The main consumer module is `forge.vm.wasm.interpret.backend`. It reexports allocator,
debug-info, exception, host-function, option, type, and watchdog contracts.
Consumers that need only a leaf type may import its owning module directly.

The backend's runtime/host-conversion partitions are implementation units of
the module graph; their source is installed where CMake needs it to rebuild
module BMIs. Partition names and private engine headers are not public import
contracts. The retired donor parser, visitors and machine-code writer are no
longer part of this module graph.

The VM and WASM family roots do not expose targets or aggregate modules.

## Retained Numeric And FP APIs

Status at this checkpoint: numeric v2 and FP/TLS APIs are restored in source;
independent numeric review is CLEAR, with the P2 finding closed. The pre-legacy-fix R5
full ON source/build-tree checkpoint passed 72/72 selected CTests on Darwin
ARM64 in 74.27 seconds, with zero failures/errors/skips/disabled tests and no
expected-failure masking. It covers core, interpreter/JIT, nine public-profile
cases and 47 AOT/session/cleanup registrations; helper/session checks select the
current build-tree compiler helper. Unit and spec report 288 and 4856 cases,
not assertion counts. All 290 source files matched that checkpoint's freeze;
this was not the entire root test suite.

The earlier LLVM-free interpreter-only run remains a separate 10/10 PASS
checkpoint (165 unit / 2428 spec cases). The later frozen F6 ON checkpoint
passes 76/76 selected tests, installed AOT/session 5/5 and the direct installed
interpreter/JIT consumer. Its separate runtime-OFF delivery passes 14/14 selected
tests, installed AOT/session 2/2 and direct interpreter smoke, with no LLVM in
resolved runtime dependencies. Spine passes three contract gates and one
293-assertion process case; its other process cases were filtered. These are
local F6 results, not automatic qualification of the later publication branch,
wider Turbine or Linux/x86/remote CI.

The retained surfaces live in `forge::vm::wasm::interpret`:

```cpp
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.floating_point_environment;
```

The backend module exposes
`expr_value execute_numeric(opcodes, std::span<const expr_value>)`, using the
existing `opcodes` and `expr_value` contracts reexported through `types`. There
is no separate numeric public module. The second import owns
`floating_point_environment`; importing the backend alone does not import that
guard.

`floating_point_environment` scopes only SoftFloat state: rounding mode,
tininess detection, exception flags and extF80 precision. It saves the caller's
four values, selects nearest-even / after-rounding / zero flags / precision 80,
and restores the saved values on normal or C++ exceptional exit, including
nested scopes. It does not change the native hardware FP environment, establish
WAMR's guest FP boundary or provide signal/trap transport. A recovery jump must
not bypass its destructor.

SoftFloat definitions compiled as C and declarations consumed from C or C++
must use the same TLS (thread-local storage) ABI. The restored build applies
`THREAD_LOCAL=__thread` as a PUBLIC requirement of the existing SoftFloat target,
including its installed internal target. Rebuild the library and all consumers
together; do not mix old non-TLS objects/archives with new TLS declarations.

The numeric adapter executes exactly 62 FP/comparison/conversion
operations through one fixed, lazily prepared WAMR module and a fresh interpreter
context for each call. It uses the existing WAMR admission, numeric execution and
FP boundary, not an EOS visitor, a second interpreter or a keyed module cache.
It does not read or mutate SoftFloat state and does not require the public guard
to establish its guest arithmetic behavior.

Arithmetic NaNs follow the approved canonical WAMR profile
(`0x7fc00000` / `0x7ff8000000000000`). Reinterpretation and raw sign/bit
operations retain their specified payload and signed-zero behavior; this is not
historical EOS arithmetic-NaN payload parity.

This is not a hard-real-time or zero-allocation numeric primitive. Cold
preparation and per-call context lifecycle can allocate and have variable
latency; no fixed latency, exact per-call allocation budget or old SoftFloat
performance guarantee is promised. The checkpoint above covers only its recorded
Darwin source/build-tree checks and separately bounded runtime-package evidence.
Rebuild/reinstall after source changes and complete the remaining full-package,
platform and product gates; earlier package results do not qualify a later API
or installed-helper snapshot.

## Execution Model

A production execution lane consists of four objects:

1. canonical WASM bytes owned or fetched by the product;
2. a `backend` that owns the parsed module and execution context;
3. a `wasm_allocator` that owns guarded guest linear memory;
4. a host object that owns the capabilities visible to that invocation.

Backend construction prepares and validates WASM. Context-owning constructors
can also initialize it and execute its start function. For untrusted modules,
use preparation without an attached context, then `with_invocation` so the
deadline covers initialization and start as well as the exported call.
The input is already WASM, not source code: WAMR interprets it or LLVM JIT
compiles it to native code inside the process.

Legacy backend operations and execution contexts require external synchronization.
`with_invocation` rejects overlapping scoped calls rather than queuing them;
it does not make concurrent legacy operations safe. Give every concurrently
executing lane its own compiled module, context, linear memory, mutable host
state, and deadline guard.
Host-function registration is static per `registered_host_functions` type and
must finish during process startup before backend construction.

## Cookbook: Load Canonical WASM Bytes

`wasm_code` is `std::vector<std::uint8_t>`. File, database, network, signature,
and content-digest policy belongs to the product. Always authenticate the bytes
before constructing a backend when code identity is security-sensitive.

```cpp
#include <filesystem>
#include <fstream>
#include <stdexcept>

import forge.vm.wasm.interpret.types;

forge::vm::wasm::interpret::wasm_code read_wasm(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
   if (!input) {
      throw std::runtime_error{"cannot open WASM module"};
   }

   const auto end = input.tellg();
   if (end < 0) {
      throw std::runtime_error{"cannot determine WASM module size"};
   }

   auto code = forge::vm::wasm::interpret::wasm_code(static_cast<std::size_t>(end));
   input.seekg(0, std::ios::beg);
   if (!code.empty() &&
       !input.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()))) {
      throw std::runtime_error{"cannot read complete WASM module"};
   }
   return code;
}
```

Do not pass a temporary view into mutable or short-lived storage. A backend
copies module structures into its own arena, but the constructor must receive
valid bytes for the complete parse.

## Cookbook: Register Host Functions Once

Host functions are the guest's capability boundary. Keep the host type narrow,
validate application policy before exposing side effects, and register the
exact module/name/signature expected by the WASM imports.

```cpp
#include <cstdint>
#include <string_view>

import forge.vm.wasm.interpret.backend;

namespace wasm = forge::vm::wasm::interpret;

struct invocation_host {
   std::uint64_t id = 0;

   std::uint64_t invocation_id() const {
      return id;
   }

   void write_log(wasm::span<const char> text) {
      const auto message = std::string_view{text.data(), text.size()};
      // Send message to the application logger. Do not retain text.data().
   }
};

using host_functions = wasm::registered_host_functions<invocation_host>;

void register_host_functions() {
   static const bool registered = [] {
      host_functions::add<&invocation_host::invocation_id>("env", "invocation_id");
      host_functions::add<&invocation_host::write_log>("env", "write_log");
      return true;
   }();
   static_cast<void>(registered);
}
```

`span<T>` arguments consume two WASM operands: an offset and an element count.
The default converter validates offset, multiplication, addition and the final
range against the currently active linear-memory byte count before calling the
host method. C strings use a bounded terminator search within the same active
range. A mutable span writes directly into guest memory.
Never retain a guest pointer or span after the host callback returns.

For potentially unaligned structured data, use
`argument_proxy<T*, LegacyAlign>` or `argument_proxy<span<T>, LegacyAlign>`.
The proxy copies unaligned trivially-copyable values and copies mutable values
back on destruction. Prefer scalar arguments or byte spans for stable ABIs;
passing a native C++ struct requires an explicitly proven layout and endian
contract.

An unresolved import, a mismatched signature, or a non-function import is
reported as `forge::vm::wasm::interpret::exceptions::link` during backend
construction. Registration mutates a process-wide mapping for the selected
host-function type; do not add mappings while other threads construct or run
backends.

## Cookbook: Interpreter With A Deadline

Use a bounded validation profile and a deadline for untrusted code. Existing
option names are subject to the versioned WAMR capacity profile, not the full
historical donor range. Every consensus-visible limit must be stable across
all participating nodes.

```cpp
#include <chrono>
#include <cstdint>

import forge.vm.wasm.interpret.backend;

namespace wasm = forge::vm::wasm::interpret;

using host_functions = wasm::registered_host_functions<invocation_host>;
using interpreter = wasm::backend<
   host_functions,
   wasm::interpreter,
   wasm::compatibility_options>;

void initialize_vm() {
   register_host_functions();
}

void execute(wasm::wasm_code code) {
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto vm = interpreter{pointer, code.size(), nullptr,
      wasm::compatibility_options{}, true, false};
   vm.validate();
   auto host = invocation_host{.id = 42};
   vm.with_invocation(&host, wasm::compatibility_options{},
      std::chrono::milliseconds{50}, {}, [&] {
         vm(host, "env", "apply", std::uint64_t{42}, std::uint64_t{7});
      });
}
```

Call `initialize_vm()` once during process startup, before worker threads begin
constructing backends. The invocation path must not mutate the process-wide host
function registry.

`watchdog` starts one helper thread for each `scoped_run`. `backend::timed_run`
is templated, so a high-concurrency service may supply a product scheduler-backed
watchdog with the same `scoped_run(callback)` guard contract. `null_watchdog`
means unbounded execution and is not appropriate for untrusted production code.

On expiry, the callback requests cooperative cancellation through WAMR's
interrupt mechanism. `timed_run` reports
`forge::vm::wasm::interpret::exceptions::timeout`. Do not catch and ignore this exception
inside the timed callable.

## Cookbook: Read A Return Value

`call_with_return` returns an optional WASM stack value. Check it before reading
the typed representation.

```cpp
auto result = vm.call_with_return(host, "env", "balance_of", std::uint64_t{42});
if (!result) {
   throw std::runtime_error{"balance_of returned no value"};
}
const auto balance = result->to_ui64();
```

Use `to_ui32`, `to_ui64`, `to_f32`, or `to_f64` according to the validated
export signature. The pinned WAMR profile canonicalizes arithmetic NaNs and
establishes nearest-even rounding with gradual underflow at execution/host-call
boundaries; bitwise operations retain their specified payload behavior. The
host still owns the semantics of its native functions. Numeric qualification
is required separately for each runtime mode and architecture.

## Cookbook: Reset Between Isolated Invocations

For a backend prepared without an attached context, use a new bounded invocation
for each isolated call. Each scope initializes fresh memory/globals and runs the
start function under the same deadline as the exported call.

```cpp
for (const auto receiver : {first_receiver, second_receiver}) {
   vm.with_invocation(&host, wasm::compatibility_options{},
      std::chrono::milliseconds{50}, {}, [&] {
         vm(host, "env", "apply", receiver);
      });
}
```

For manually managed contexts, `initialize` also executes start and must be
inside the caller's timed scope, not between two timed calls. Do not call it
concurrently with execution. If a product intentionally
wants stateful WASM memory across calls, it must define and test that lifecycle
instead of using the isolated-invocation recipe.

## Cookbook: Exclusive Invocation Scope

For isolated calls, prepare a backend using the pointer/size constructor with
context ownership disabled as above. Validate the module and the product's
import/export policy, then use:

```cpp
vm.with_invocation(&host, options, std::chrono::milliseconds{50}, stop_token, [&] {
   vm.call(host, "env", "apply", receiver);
});
```

The zero-argument body runs after initialization and the start function. One
watchdog covers initialization, start, and body. Each scope owns fresh context,
globals, and linear memory, including active data initialization. The supported
MVP table is immutable from guest code; its compiled element initialization is
retained without sharing the module between concurrent instances. Context and
allocator references are detached on every exit, after the watchdog is joined.

Admission rejects overlapping or reentrant scopes with `exceptions::busy`,
shared compiled-module ownership with `exceptions::shared_instance`, and an
owned or already attached context/allocator with `exceptions::invocation_state`.
`share()` also rejects an active invocation. Independent concurrent calls must
use independently compiled modules with distinct code allocators.

Deadline expiry reports `exceptions::timeout`; requested stop reports
`exceptions::canceled`. Guest execution traps, deadline, and cancellation permit
a later scope with fresh state. An exception escaping a host callback, even one
using a VM exception type, or an unknown native body failure marks the instance
unusable. Subsequent scopes and sharing report `exceptions::unusable`; discard
that backend. A caught host exception still poisons the scope and is rethrown at
its boundary. The original host exception takes precedence over racing stop or
deadline. This is not rollback for external host side effects.

Cancellation uses WAMR interrupt checks, not executable-page disabling or native
thread preemption. Host callbacks and the native body must return in bounded
time; a stop request cannot forcibly interrupt blocking host work. Legacy
`timed_run`, `initialize`, and externally attached-context APIs remain available
with their original caller-managed lifetime and synchronization obligations.

## Legacy Timed Scopes And Recovery

Status: V3 is applied and qualified within the frozen F6 local ON and OFF
checkpoints. Source review is P1/P2 CLEAR; installed and unchanged Spine smoke
checks passed. Broader product and cross-platform release acceptance remains
separate from those bounded results.

Ordinary `timed_run` overloads, including the absolute-deadline overload with
`interrupt_options`, retain f6 caller-managed lifetime behavior. A timed scope
is a budget guard, not an implicit VM reset or rollback boundary. A transported
host exception alone does not permanently poison this legacy backend. After
the failed scope closes, the caller can reinitialize it inside a new bounded
scope before guest reuse. This does not relax strict `with_invocation` above.

`initialize(host, options)` retires the old VM instance and its linear-memory
borrow, clears the closed-instance failure state, and creates fresh globals,
memory/data initialization and host association before running start. It does
not destroy the caller-owned host or undo host/application/DB effects. The
application still owns its transaction and rollback policy.

Within an active timed/invocation scope, the first recorded host failure
survives raw context reset, clear and reinitialization and retains precedence
over racing cancellation/deadline. A failure from a closed scope must not
override a future call's pre-cancellation or be rethrown by a new empty scope
with no new failure; the new scope's own admission and interruption rules apply.

## Cookbook: Validation Without Execution

`null_backend` parses and validates a module without executing it. This is useful
at code-admission time. Runtime construction must still validate the exact bytes
that will execute; a prior validation result is not a substitute for matching a
trusted code digest.

When using an externally owned context (`exec_ctx_by_backend = false`), call
`backend::validate()` after construction and before admitting prepared code.
It checks the parsed module's instantiation error and active data-segment bounds,
without allocating an execution context or running the start function. Parsing
and import resolution still belong to backend construction. This method does
not enforce a product's required exports or import policy.

```cpp
using validator = wasm::backend<
   std::nullptr_t,
   wasm::null_backend,
   wasm::compatibility_options>;

void validate(wasm::wasm_code& code) {
   auto parsed = validator{code, static_cast<wasm::wasm_allocator*>(nullptr)};
   static_cast<void>(parsed.get_module());
}
```

Construction throws typed parse, allocation, section-length, pointer, or link
exceptions. Keep admission failures separate from execution traps in product
metrics and error handling.

## Cookbook: Optional LLVM JIT

WASM binaries are architecture-neutral. In an LLVM-JIT-enabled build, `jit` asks the
node process to translate the already validated WASM into native code when the
backend is constructed. It does not rebuild the contract source and it does not
change the uploaded WASM artifact.

```cpp
#if FORGE_VM_WASM_INTERPRET_HAS_JIT
using native_engine = wasm::backend<
   host_functions,
   wasm::jit,
   wasm::compatibility_options>;

auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
auto vm = native_engine{pointer, code.size(), nullptr,
   wasm::compatibility_options{}, true, false};
vm.validate();
auto host = invocation_host{};
vm.with_invocation(&host, wasm::compatibility_options{},
   std::chrono::milliseconds{50}, {}, [&] {
      vm(host, "env", "apply", std::uint64_t{0});
   });
#endif
```

JIT requires the configured LLVM version and supported host target. The current
source selects WAMR LLVM JIT for x86_64 and ARM64; native ARM64 probes are not
evidence of an x86_64 or Linux release pass. Never silently select a different
backend: policy must be explicit and admitted modules must produce equivalent
observable results across qualified modes.

Select `backend<Host, jit_profile, Options, profile_instr_map>` to use the existing
`get_debug().translate(pc)` diagnostic surface. The default debug type remains
`null_debug_info`; selecting a profiled execution mode does not change that
template default. `prepared_module::get_debug()` returns an owning observer
that can outlive the backend and pins its code, map and preparation owner.
Translation is allocation-free and returns `UINT32_MAX` for unknown addresses,
including host code, gaps and another module's code. Stop and join sampling
before moving, replacing or destroying that observer. Native maps are immutable:
`relocate()` rejects them rather than pretending that LLVM-owned code moved.
Product code owns the sampling driver; the VM owns mapping and lifetime.
The [current qualification](../../../../docs/donors/forge-wamr-dev-integration-20261018.md)
separates the implementation and static review from platform/runtime acceptance.

## Allocator Ownership

The allocator family is specialized; it is not a general application allocator.

| Allocator | Role | Production ownership |
|---|---|---|
| `wasm_allocator` | Guarded guest linear memory, committed in 64 KiB WASM pages | One long-lived instance per execution lane; never share concurrently; call `reset()` between invocations |
| `stack_allocator` | Optional native stack with room for host and signal handling | Low-level runtime primitive; RAII-managed |
| `bounded_allocator` | Fixed monotonic byte arena with explicit reset/free semantics | Low-level parser/tooling primitive |
| `contiguous_allocator` | Page-backed contiguous arena that grows by remapping | Low-level tooling primitive |
| `fixed_stack_allocator<T>` | Guarded fixed stack region | Low-level runtime primitive; not the normal guest-memory owner |

`wasm_allocator` reserves a large guarded virtual-address region; it does not
commit the whole region as resident memory. WASM pages become readable/writable
through `mprotect` as the guest memory grows. Guard pages and the surrounding
signal machinery are part of bounds enforcement.

The backend stores a non-owning pointer to `wasm_allocator`. The allocator owns
its guarded mapping and releases it on destruction; `free()` is an idempotent
early-release operation. Therefore:

- construct the allocator before the backend;
- destroy all referencing backends before an explicit `memory.free()`;
- do not copy the allocator;
- do not reuse it concurrently;
- use `reset(page_count)` or backend `initialize` for a new isolated run, not
  `free()` followed by reuse.

A standalone `wasm_allocator::borrow` is an exclusive manual reservation, without
context idle-retirement support. The allocator must outlive the borrow and every
`borrow::activity` guard. Release manual borrows and finish active executions and
memory transitions before `free()` or allocator destruction: these `noexcept`
lifetime operations require quiescence, unlike reset operations that can reject
active use with `exceptions::busy`.

`growable_allocator` and `jit_allocator` are retired. WAMR preparation and native
mappings are accounted by the engine integration; consumers should not rebuild
the donor executable arena. See the exact
[source API changes](../../../../docs/releases/unreleased-wamr-bounded-profile.md#legacy-parser-and-allocator-api-break).

## Exceptions

Stable exception categories live in `forge::vm::wasm::interpret::exceptions`:

- `parse`, `section_length`, and `invalid_element` for malformed modules;
- `link` and `unsupported_import` for host ABI failures;
- `memory`, `pointer_out_of_bounds`, `stack_memory`, and `allocation` for
  guarded-memory/runtime failures;
- `timeout` for deadline interruption;
- `canceled` for stop-token interruption;
- `busy`, `shared_instance`, and `invocation_state` for rejected scoped admission;
- `unusable` for a backend permanently rejected by strict `with_invocation`
  after a host/native failure;
- `exit` for an explicit VM exit;
- `interpreter` for execution traps not represented by a narrower category.

Catch the narrow category at the layer that can make a policy decision. Do not
convert all failures into a generic success/false result, and do not expose raw
exception text as a consensus-visible value.

```cpp
try {
   vm.timed_run(deadline, [&] { vm(host, "env", "apply", receiver); });
} catch (const wasm::exceptions::timeout&) {
   // Abort local work; wall-clock expiry is not a deterministic guest result.
} catch (const wasm::exceptions::memory&) {
   // Treat as a guest execution trap.
} catch (const wasm::exceptions::link&) {
   // Treat as deployment/configuration failure, not a retryable invocation.
}
```

## Parallel Execution And Caching

The library has no scheduler and no product code cache. A production host should:

1. authenticate bytes and select the consensus-visible option profile;
2. register host functions once during startup;
3. cache code/backend artifacts by a verified content digest in the product
   layer;
4. assign independent backend/context, host state, and linear memory to each
   concurrent lane;
5. wrap every untrusted invocation in a deadline;
6. reset or discard the execution lane after traps according to product policy;
7. never run side-effecting host functions without the product transaction or
   rollback boundary already active.

`backend::share` and externally supplied contexts are low-level donor mechanisms
for specialized pools. They are not an automatic thread-safe cache. Use them
only after the product proves context isolation, allocator ownership, reset
behavior, and failure cleanup under concurrency.

## Options And Compatibility

`default_options` and `compatibility_options` retain the consumer option
vocabulary, including memory pages, call depth, symbols, sections, locals,
tables and control depth. The common WAMR profile imposes additional admitted
capacity and numeric constraints; the old names are not a promise to accept
every historical module. Unsupported configuration is rejected explicitly.

`max_control_depth` counts nested `block`, `loop`, and `if` frames; `else` does
not open another frame. The compatibility profile also preserves the donor's
`max_legacy_nested_structures` accounting, which counts `else`,
because changing that limit would change Spring/Antelope module acceptance.
Product-specific profiles should use `max_control_depth` for normal nesting
policy and should not opt into the legacy counter.

Products may define another options type with the same named static members or
use the runtime `options` record. Do not default-construct `options` and leave
its scalar fields uninitialized. Fill every field from validated policy before
passing it to a backend. In a blockchain, changing any execution or validation
limit is a protocol change and must be activated consistently.

## Boundaries

This library owns only neutral WASM mechanics. It does not own blockchain
controller behavior, contract policy, state storage, code cache policy,
resource billing, transaction execution, or product-specific host functions.
Those belong to consuming products.

The donor's `memory_dump` and sampling-driver headers are not Forge services.
Product sampling orchestration stays outside the VM, but the public
instruction-map behavior required by `jit_profile` belongs to this leaf, not to
a second product-owned parser or relocator.

## Production Checklist

- Verify the code digest before validation and again before execution/cache use.
- Use a bounded options profile; never pass uninitialized runtime limits.
- Register imports once before worker threads start.
- Keep one backend/context and `wasm_allocator` per concurrent execution lane.
- Use guarded spans/proxies and never retain guest pointers.
- Execute untrusted code under a deadline.
- Reset long-lived execution-lane allocators between invocations.
- Keep product state changes inside a transaction owned by the caller.
- Test interpreter/JIT equivalence for every supported architecture and release.
- Run donor mapping, spec, sanitizer, fuzz-build, and relocatable-package gates.

## Tests

The historical test tree is generated from the pinned EOS VM donor suite and uses
Boost.Test. A manifest records donor source hashes, test cases, assertion
counts, backend matrices, and fixture hashes. The mapping gate rejects changes
outside the explicit namespace, symbol, exception, macro, and assertion-framework
translations. Replacement-specific regression tests additionally cover the
approved API/profile changes, validation-only behavior, intrinsic calls,
allocator handoff, cancellation/reuse and native preparation budgets. Historical
donor evidence is not a passing result for the current source tree.

The manual `VM WASM` workflow and its donor oracle are historical baseline
infrastructure, not proof that the replacement has passed every platform/mode.
Track fresh results in the compatibility checkpoint. Catch2 remains donor-test
infrastructure and is not a Forge package dependency.

The pinned donor revision has two modern-toolchain build defects: its signal
header neither includes its allocator definitions nor accepts the null
allocators used by its own tests, and its host result conversion inspects
forwarding-reference types without removing references. The oracle applies
hash-checked compatibility patches to those two headers; the donor test tree
remains unchanged and is verified before it is built.

`forge_vm_wasm_interpret_spec_test_generator` is an excluded-from-all maintenance tool
copied from the pinned donor. `tests/vm_wasm/regenerate_spec_test.py` maps its
JSON output through the same checked Forge/Boost transformation. The optional
`FORGE_VM_WASM_INTERPRET_ENABLE_FUZZ_TESTS=ON` target builds a validation-only
harness with Clang libFuzzer and AddressSanitizer; it must not run guest start
functions. The target's flags alone do not establish sanitizer instrumentation
of the entire runtime. The harness is not installed.

See `PROVENANCE.md`, `THIRD_PARTY_LICENSES`, and `LICENSE.eos-vm` for source
lineage and licensing.
