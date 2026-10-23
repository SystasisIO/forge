# Installed optimizing component fixture

This is a standalone consumer of installed `Forge::` targets. It does not add
Forge production sources, use build-tree Forge targets or include private engine
headers. Canonical test sources are reused without fixture-specific edits; the
v2 freeze follows the parent's reviewed `format-r1` postimages:

- [export_name_tests.cpp](../vm_wasm_optimizing/export_name_tests.cpp) builds as two
  separate executables. Only the producer gets
  `FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE=1` and links
  `Forge::forge_vm_wasm_compiler`. The runtime links installed
  `Forge::forge_vm_wasm_optimizing` and `Forge::forge_asio`, decodes the producer's
  requests/artifacts,
  prepares native code and invokes every export, including hook controls.
- [session_tests.cpp](../vm_wasm_optimizing/session_tests.cpp) builds unchanged
  against `Forge::forge_vm_wasm_optimizing` and `Forge::forge_asio`. Its includes
  are public modules,
  Boost.Asio and standard/POSIX headers. It exercises the real helper, admission,
  cold/warm/disk cache, cancellation/reuse, quotas, eviction, tamper/restart,
  retained module/weak-owner charges and shutdown. It is not a replacement for
  the private phase-probe or fault-injected cleanup tests.
- [heavy_fixture.cpp](../vm_wasm_optimizing/heavy_fixture.cpp) and
  [compile_fixture.cpp](../vm_wasm_optimizing/compile_fixture.cpp) generate the
  unchanged heavy-8192 request/artifact and multi-result negative fixture. The
  generator and compiler run in separate processes from the session consumer.

## Producer-enabled qualification

Configure with an explicit installed `Forge_DIR` (or the parent's generated
package consumer seed cache), the supported compiler/SDK, LLVM 22.1.8 discovery,
`FORGE_PACKAGE_OPTIMIZING_HELPER` pointing to the installed
`bin/forge-wasm-compiler`, and a finite, explicitly approved
`FORGE_WAMR_COMPILER_TEST_AS_BYTES`. No default address-space waiver is provided.
Clang package discovery may be disabled; the producer requires LLVM. The project
enables both C and CXX: LLVM 22.1.8 `FindFFI` selects a C probe when the runner
passes `CMAKE_C_COMPILER`, which requires the C language to be enabled.

Build only this consumer graph with `cmake --build <consumer-build> -j2`, then run
`ctest --test-dir <consumer-build> --parallel 1 --output-on-failure --no-tests=error`.
CTest fixtures generate the export-name and session artifacts before their
runtime tests. Session timeout remains 900 seconds and the canonical consumer's
budgets/assertions are unchanged. Five small executable targets are built: the
export producer/runtime, session WASM generator/compiler and session consumer.

`verify_installed.cmake` is the parent registration entry point. It
configures this standalone consumer, builds it with an explicit two-job limit,
then runs only its serial tests and saves log/XML. It does not install or build
the parent. The parent must finish installation, including the CLI, first.

## Runtime-only / LLVM-free qualification

Set `FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER=OFF`. The compiler component and all
producer/generator targets disappear. Supply the completed producer run's
`FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX` and, when session is enabled,
`FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR`; missing files fail configure.
Artifacts are not synthesized or decoded by a new implementation.

For LLVM-free runtime discovery use a package built with runtime LLVM JIT OFF,
and set both `CMAKE_DISABLE_FIND_PACKAGE_LLVM=ON` and
`CMAKE_DISABLE_FIND_PACKAGE_Clang=ON`. A JIT ON package legitimately requires
LLVM even without the producer; this fixture must not bypass that dependency.
Producer/runtime pins, target and compiler-profile identity must match. The
runtime's canonical artifact checks remain authoritative.

`FORGE_PACKAGE_OPTIMIZING_BUILD_SESSION=ON` is the default. This branch requires
UNIX and an existing, explicit helper executable. A runtime-only executable can
remain LLVM-free while its separate installed compiler helper uses LLVM. Setting
the session option OFF provides only the export-name runtime gate and must not
be reported as helper/session/cache acceptance.

## Evidence Boundary

Current R5 installed qualification is **5/5 CTests PASS**, zero failures/skips,
in 7.15 seconds, after a fresh nested producer/runtime/session build. See the
[2026-10-18 report](../../docs/donors/forge-wamr-dev-integration-20261018.md).
Independent link-map review confirms one WAMR runtime provider per executable:
compiler-role runtime plus separate codegen in both producers, execution owner
only in runtime/session. This Darwin ARM64 Release compiler-ON/JIT-ON result
does not qualify LLVM-free delivery, other platforms or product processes.

The four `source-freeze-package-v1` through `v4` manifests remain unchanged
historical snapshots, including their pre-run pending statuses and README
hashes. This README is a post-run documentation update, not a compiled-source
change or a new source freeze. Root registration and the production build graph
are unchanged by this update.

### V4 Explicit Asio Consumer Dependency

[source-freeze-package-v4.json](source-freeze-package-v4.json) follows V3 without
overwriting it. The actual installed R3 build failed at the runtime source's
direct `import forge.asio.blocking`: the compiler's module map did not include
Blocking through the indirect optimizing dependency.

This fixture now requests package component `asio` and directly links
`Forge::forge_asio` to the runtime and session targets, which themselves import
Asio. This follows the existing installed Asio component fixture pattern.
Producer links/macros, link maps, quotas and canonical test sources are unchanged.
No umbrella target, manual BMI path, engine patch or production CMake change is
introduced. V4 subsequently passed installed qualification and independent
review as recorded above; its frozen manifest retains the earlier pending state.

### V3 Link-Map Evidence

[source-freeze-package-v3.json](source-freeze-package-v3.json) preserves both
earlier manifests and records the link-map-only CMake enhancement plus this
README update. V2 received Bohr's scoped clear with all 15 hashes current;
V3's pending review/runtime fields describe its pre-run freeze, not the current
R5 qualification recorded above.

Apple uses the existing `LINKER:-map` pattern and Linux uses `LINKER:-Map`.
Each enabled target writes its own file directly in `CMAKE_CURRENT_BINARY_DIR`:

- `forge_package_vm_wasm_optimizing_producer.linkmap`
- `forge_package_vm_wasm_optimizing_runtime.linkmap`
- `forge_package_vm_wasm_optimizing_session_producer.linkmap`
- `forge_package_vm_wasm_optimizing_session.linkmap`

Producer/session options still determine which targets exist. Runtime-only
configuration adds no producer targets just to obtain maps. The standalone
heavy-WASM generator needs no map because it links no Forge/WAMR runtime.
Budgets, helper arguments, canonical sources and runtime behavior are unchanged.

For each qualification, inspect the contributing archive/object entries for
WAMR runtime, loader and allocator C symbols in each map, following the
[compiler isolation contract](../../libraries/vm/wasm/compiler/README.md#isolation).
The two producers must use the compiler-role runtime provider; the runtime and
session executables must use the execution owner. Compiler codegen and LLVM
archives are not by themselves a second runtime provider. Reject a map where
objects from both runtime roles contribute WAMR runtime definitions. Final
symbol counts alone cannot detect static-archive object mixing. Keep each map
with its exact binary/source identity; merely enabling map output is not a
sole-provider PASS.

### V2 Freeze And Formatting Synchronization

[source-freeze-package-v2.json](source-freeze-package-v2.json) supersedes the
current-input claims of [v1](source-freeze-package-v1.json), not its historical
bytes. V1 is preserved at SHA-256
`e0861be5c4d8a8dbf3850f2506a2e52f488d014e6984921e229b3840de06203f`.
V2 records the C-language fix, all 15 actual package/input/artifact hashes and
Bohr's two-P2 review reference. At the V2 freeze the runner/root patch were
unchanged and the root patch was not yet applied. The parent subsequently
reported Bohr's V2 scoped clear and applied registration after completing R2.
V3 snapshots that applied root state without modifying the root patch or CMake.

The five refreshed canonical hashes are `export_name_tests.cpp`,
`session_tests.cpp`, `compile_fixture.cpp`, `heavy_fixture.cpp` and
`export_name_fixtures.hxx`. Each matches the parent's `format-r1` manifest
postimage; no canonical source was reverted or edited by this package fix.
The [format-r1 review](../../../../artifacts/wamr-dev-integration-20261018-r5/format-r1/review.md)
is a separate static formatting review, not installed runtime qualification.

The current `capacity_profile.cppm` SHA-256 is
`9bd78b18bd271d392cc1ed6b075c7d79f781bafea3f88e164bace98f354f53df`,
matching `format-r1`. Although only the namespace-closing comment changed,
this file participates in `FORGE_WAMR_CAPACITY_IDENTITY`. Fresh helper/runtime
and producer artifacts must use the new matching identity; do not reuse old
sidecars or relabel pre-format binary results as v2 acceptance. WAMR pin remains
`12d1f906668cde93c03e7d864e71ee1e10dfb194`.

The interpret package fixture separately exercises real `jit_profile` execution,
`get_debug()`, an owning value observer moved after backend teardown and unknown
PCs. With JIT enabled, it also builds and runs all canonical public profiling
tests against the freshly installed, relocated interpreter target. These retain
their original sampling timers and assertions for positive native-PC mapping,
signal delivery, cancellation and observer lifetime.
