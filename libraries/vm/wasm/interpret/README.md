# forge_vm_wasm_interpret

`forge_vm_wasm_interpret` is Forge's native WebAssembly virtual machine. It is a C++23
module port of AntelopeIO EOS VM and preserves the donor parser, validator,
interpreter, host-function ABI, guarded memory, watchdog, deterministic
SoftFloat operations, and x86_64 just-in-time compiler.

This is a low-level VM library. It executes architecture-neutral WASM bytes;
the consuming product owns contract selection, code caching, scheduling,
state access, resource billing, and the host functions exposed to a guest.

## Package Integration

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

Parser, visitor, signal, execution-context, SoftFloat, and native-code-generator
components are non-reexported partitions of `forge.vm.wasm.interpret.backend`. Their
source units are installed because the template backend needs them when CMake
builds module BMIs. Partition names are not public import contracts.

The VM and WASM family roots do not expose targets or aggregate modules.

## Execution Model

A production execution lane consists of four objects:

1. canonical WASM bytes owned or fetched by the product;
2. a `backend` that owns the parsed module and execution context;
3. a `wasm_allocator` that owns guarded guest linear memory;
4. a host object that owns the capabilities visible to that invocation.

Backend construction parses, validates, instantiates the module, initializes
globals and linear memory, resolves imports, and executes the WASM start
function when present. Calling an export does not compile a source-language
contract. The input is already WASM. The interpreter translates it to donor
bitcode; the x86_64 JIT translates it to native code inside the process.

The backend and its execution context are not concurrent objects. Do not call
one backend from multiple threads. Give every concurrently executing lane its
own backend/context, `wasm_allocator`, mutable host state, and deadline guard.
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

Use a bounded validation profile and a deadline for untrusted code. The
compatibility profile preserves the donor limits used by Spring/Antelope. A
different product may define its own options type, but every consensus-visible
limit must be stable across all participating nodes.

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
   auto memory = wasm::wasm_allocator{};
   {
      auto host = invocation_host{.id = 42};
      auto vm = interpreter{code, host, &memory};
      auto deadline = wasm::watchdog{std::chrono::milliseconds{50}};

      vm.timed_run(deadline, [&] {
         vm(host, "env", "apply", std::uint64_t{42}, std::uint64_t{7});
      });
   }

   // wasm_allocator owns and releases its guarded mapping.
}
```

Call `initialize_vm()` once during process startup, before worker threads begin
constructing backends. The invocation path must not mutate the process-wide host
function registry.

`watchdog` starts one helper thread for each `scoped_run`. `backend::timed_run`
is templated, so a high-concurrency service may supply a product scheduler-backed
watchdog with the same `scoped_run(callback)` guard contract. `null_watchdog`
means unbounded execution and is not appropriate for untrusted production code.

On expiry, the callback disables the backend's code pages. The guarded signal
path interrupts execution and `timed_run` reports
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
export signature. Forge uses deterministic SoftFloat for guest floating-point
instructions. The host still owns the semantics of native floating-point
values passed into or returned from host functions.

## Cookbook: Reset Between Isolated Invocations

Backend construction performs the first initialization. To reuse that backend
for another isolated invocation, call `initialize` between invocations. It
resets the operand-stack capacity, linear memory, data segments, and globals,
then executes the module's start function again when one is present.

```cpp
vm.timed_run(deadline, [&] { vm(host, "env", "apply", first_receiver); });

vm.initialize(host);
vm.timed_run(deadline, [&] { vm(host, "env", "apply", second_receiver); });
```

Do not call `initialize` concurrently with execution. If a product intentionally
wants stateful WASM memory across calls, it must define and test that lifecycle
instead of using the isolated-invocation recipe.

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

## Cookbook: x86_64 JIT

WASM binaries are architecture-neutral. On x86_64, selecting `jit` asks the
node process to translate the already validated WASM into native code when the
backend is constructed. It does not rebuild the contract source and it does not
change the uploaded WASM artifact.

```cpp
#if defined(__x86_64__)
using native_engine = wasm::backend<
   host_functions,
   wasm::jit,
   wasm::compatibility_options>;

auto memory = wasm::wasm_allocator{};
{
   auto host = invocation_host{};
   auto vm = native_engine{code, host, &memory};
   vm.timed_run(deadline, [&] { vm(host, "env", "apply", std::uint64_t{0}); });
}
memory.free();
#endif
```

The JIT is intentionally unavailable on ARM64 because the donor has no ARM64
native code generator. Use the interpreter there. Never silently select a
different backend in consensus code: backend policy must be explicit and both
backends must produce identical observable results.

`jit_profile` plus `profile_instr_map` records a native-code-to-WASM instruction
map for profiling. It has additional memory cost and should be selected only by
an explicit diagnostics profile.

## Allocator Ownership

The allocator family is specialized; it is not a general application allocator.

| Allocator | Role | Production ownership |
|---|---|---|
| `wasm_allocator` | Guarded guest linear memory, committed in 64 KiB WASM pages | One long-lived instance per execution lane; never share concurrently; call `reset()` between invocations |
| `growable_allocator` | Arena for parsed module structures and generated code | Owned by `module`; backend manages it |
| `jit_allocator` | Process-wide executable-page pool using large virtual segments | Internal singleton used by x86_64 JIT; consumers do not allocate from it directly |
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

`growable_allocator`, `jit_allocator`, guarded containers, and stack allocators
are exported because they are part of the native EOS VM component model. Most
applications should interact only with `wasm_allocator` and let `backend` own
the rest.

## Exceptions

Stable exception categories live in `forge::vm::wasm::interpret::exceptions`:

- `parse`, `section_length`, and `invalid_element` for malformed modules;
- `link` and `unsupported_import` for host ABI failures;
- `memory`, `pointer_out_of_bounds`, `stack_memory`, and `allocation` for
  guarded-memory/runtime failures;
- `timeout` for deadline interruption;
- `exit` for an explicit VM exit;
- `interpreter` for execution traps not represented by a narrower category.

Catch the narrow category at the layer that can make a policy decision. Do not
convert all failures into a generic success/false result, and do not expose raw
exception text as a consensus-visible value.

```cpp
try {
   vm.timed_run(deadline, [&] { vm(host, "env", "apply", receiver); });
} catch (const wasm::exceptions::timeout&) {
   // Map to the product's deterministic deadline result.
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

`default_options` follows the generic donor defaults. `compatibility_options`
provides the bounded Spring/Antelope-compatible profile, including limits for
memory pages, call depth, symbols, sections, locals, tables, and control depth.

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

The donor's inactive `memory_dump` and `profile` headers are intentionally not
ported. They are not part of the active EOS VM build or public VM behavior.

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

The test tree is generated from the pinned EOS VM donor suite and uses
Boost.Test. A manifest records donor source hashes, test cases, assertion
counts, backend matrices, and fixture hashes. The mapping gate rejects changes
outside the explicit namespace, symbol, exception, macro, and assertion-framework
translations.

The manual `VM WASM` GitHub Actions workflow builds interpreter and x86_64 JIT
matrices in Debug and Release, an ARM64 interpreter lane, ASan/UBSan, a
relocatable package consumer, and the donor's original Catch2 bodies as an
oracle. Catch2 remains CI-only donor infrastructure and is not a Forge package
dependency.

The pinned donor revision has two modern-toolchain build defects: its signal
header neither includes its allocator definitions nor accepts the null
allocators used by its own tests, and its host result conversion inspects
forwarding-reference types without removing references. The oracle applies
hash-checked compatibility patches to those two headers; the donor test tree
remains unchanged and is verified before it is built.

`forge_vm_wasm_interpret_spec_test_generator` is an excluded-from-all maintenance tool
copied from the pinned donor. `tests/vm_wasm/regenerate_spec_test.py` maps its
JSON output through the same checked Forge/Boost transformation. The optional
`FORGE_VM_WASM_INTERPRET_ENABLE_FUZZ_TESTS=ON` target builds the parser harness with Clang
libFuzzer and AddressSanitizer; it is not installed.

See `PROVENANCE.md`, `THIRD_PARTY_LICENSES`, and `LICENSE.eos-vm` for source
lineage and licensing.
