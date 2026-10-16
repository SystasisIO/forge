# WASM compiler tooling

Use `forge_tooling_wasm_compiler` in an isolated compiler worker to turn owned
WASM plus its exact execution configuration into a checked native artifact.
It is not a runtime backend and must never compile inline in an invocation or
while a product database writer is open. LLVM 22.1.8 is private to this target;
runtime artifact consumers do not import or link LLVM.

```cpp
import forge.tooling.wasm_compiler.compiler;

auto artifact = forge::tooling::wasm_compiler::compiler{}.compile(request);
auto bytes = artifact.encode();
```

`request` is the real `optimizing::compile_request`: owned input, canonical
execution-options snapshot, import signatures, target and explicit output/frame
budgets. Never log its input bytes. The integer/control-flow source slice lowers
definitions and direct calls using the existing parser's typed PCs, including
result-carrying branches and in-place branch-table payloads. It emits the MVP
integer operations with explicit division traps, defined signed remainder,
masked shifts and backward-edge interruption checks. Its code-generation and
native-execution gates are recorded separately in the provenance note.
The separate memory/global slice uses the same context offsets and existing
growth callback, full-width bounds checks before pointer formation, unaligned
little-endian loads/stores, raw float bit movement and invocation-local globals.
Memory bases are reloaded after callbacks; growth limits/allocation belong to
the runtime owner, not a second compiler options record. Its eight auxiliary
macOS arm64 cases pass code generation and interpreter fixture checks; Linux
native execution and the real runtime growth/bounce gates remain NOT_RUN.
Float constants, signatures, locals and control-flow results are transported
as raw i32/i64 bits. All 62 FP/comparison/conversion operations call the
unchanged numeric ABI with existing opcode IDs and one/two raw operands. The
host callback reuses `interpret::execute_numeric`; LLVM emits no native FP
arithmetic or conversions. That shared owner uses the same interpreter
overloads/SoftFloat helpers and preserves typed conversion failures and caller
FP state. The executor must establish its host stack and whole-NULL recovery
scope before constructing callback/FP RAII. Compiler primitives on an ordinary
host stack prove neither that production bridge nor its performance.
Function imports emit ordinary generated thunks to the unchanged host callback.
Bindings use the module/name-sorted configuration ordinal and exact signature,
not a process registry index. Indirect calls reuse `module::finalize()` type
aliases for structural equivalence, including duplicate declared signatures.
The selector, null slot, type, function index and native entry are checked
before the indirect call or host effects. These source capabilities do not
establish a production host registry, guest/host bridge or whole-module runtime
readiness. Unbound/mismatched imports fail as typed local unsupported imports;
unsupported frontend features are not silently interpreted or reclassified as
chain admission failures.

Canonical targets are Linux x86-64 (`x86_64-unknown-linux-gnu`, CPU `x86-64`,
empty features) and Darwin ARM64 (`aarch64-apple-macosx11.0.0-elf`, CPU `generic`,
features `+fp-armv8,+neon`). Both use an ELF object container, but LLVM derives
the calling convention from the explicit operating-system triple. Unsupported
aliases, CPUs, feature strings and LLVM/codegen/host-ABI versions reject before
compilation. The target profile remains part of the shared Raw cache identity.
Object normalization rejects unknown/external relocations and missing or dynamic
frame metadata. Runtime preparation checks the host OS/architecture separately.
Native primitive fixtures and full executor acceptance are distinct gates;
signal handling, guarded aggregate stack, host-stack callbacks and packaging
remain outside this compiler slice.

ABI version 2 appends the single shared epoch pointer, invocation-owned
threshold and sticky reason pointer. Function/import entry and actual backward
edges use one system-scope acquire epoch load and an unsigned comparison;
only expiration enters the existing cold poll callback. No duplicate caller
poll, clock read or sticky load is added to the fast path. Threshold loads are
not invariant across callback rearm. Backing words must be pinned, correctly
aligned lock-free atomic_ref storage; all host access is atomic_ref. The runtime
owns snapshot+1 rearm, first-wins stop/deadline, saturation, real-clock checks,
and independent compute-stop publication after ticker stop. Old ABI/codegen
profiles reject through the same canonical request/artifact validator before
prepared publication. Actual O2/private diagnostic and ordinary-host-stack
primitives do not establish production host bounce, cancellation lifetime,
Linux native support or throughput parity with Spring.

Codegen version 3 distinguishes the unchanged canonical call-depth trap
`stack_overflow` (raw reason8) from insufficient LOCAL native frame allowance
`native_stack_exhausted` (new appended raw reason12). The latter is an
operational capacity failure; it must not become an objective WASM trap.
The invocation ABI remains version2/size168 and every previous raw reason is
unchanged. Exact codegen1/2 profiles reject through the SAME canonical
request/artifact validator before prepared publication. Artifact/configuration
schemas and the identity algorithm are unchanged; the full target profile
already makes the new generated semantics a different identity/cache entry.
Prior ABI2/codegen2 fixture files remain historical immutable evidence, not
current-runtime inputs. Fresh codegen3 fixtures require separate generation.

The development package installs `LICENSE.Spring` and `LICENSE.WAVM` in the
existing `Forge_NOTICES_DIR` alongside the other Forge legal notices.
`Forge_NOTICE_FILES` supplies their basenames as part of the required root-level
notice list, not the complete third-party notice subtree. Redistributors must
copy the entire `Forge_NOTICES_DIR`, including its existing subdirectories;
the basename list is for root-level validation, not a copy filter. Do not
maintain a separate compiler-license directory or fixed filename subset.

The thin installed `forge-wasm-compiler` executable calls the owning `command`
component. It accepts only the private inherited-endpoint launch envelope,
applies explicit finite CPU/absolute address-space/stack/core limits before
reading the request, and uses SAME request/artifact/Raw codecs. It checks the
actual Forge build, closed target/profile and exact acknowledgement before
LLVM initialization. The host session uses existing Boost.Process async reap,
not the default fork launcher or a managed blocking process destructor. Parent
and helper failures are typed local compilation/lifecycle failures, not guest
execution verdicts. The ordinary helper smoke gates are separate from real
resource-cap enforcement, root/package/installed-consumer and runtime gates.
