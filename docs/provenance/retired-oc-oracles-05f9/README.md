# Retired OC Oracle Sources

The maintainer approved retiring exactly five unregistered legacy OC test/export
files from the publication source graph. This is **not full oracle parity or
acceptance of the WAMR replacement**. Operational coverage gaps remain open.

## Immutable Preimages

These are byte-for-byte copies from Forge commit
`05f9edafe7c0bbd07f7bc8338907348f376a123e`, not regenerated or reformatted tests.
The old compiler source originated in
`f6b617ddae0847113eb5c75b225fdb2fde2f3b23`. Archive filenames append `.txt` so
they are provenance records, not CMake/module/fixture inputs.

- [optimizing_compiler_tests.cpp](preimages/tests/vm_wasm/optimizing_compiler_tests.cpp.txt)
- [optimizing_epoch_ir_tests.cpp](preimages/tests/vm_wasm/optimizing_epoch_ir_tests.cpp.txt)
- [optimizing_fixture_export.hpp](preimages/tests/vm_wasm/optimizing_fixture_export.hpp.txt)
- [export_runtime_fixtures.cmake](preimages/tests/vm_wasm/export_runtime_fixtures.cmake.txt)
- [old package verify_relocation.cmake](preimages/tests/package_vm_wasm_optimizing_component/verify_relocation.cmake.txt)

The [gap inventory](gap-inventory.json) records original paths, byte counts,
Git blob IDs and SHA-256 values, all 27 legacy case names, the current 47-case
AOT source inventory and their qualified comparison. Existing donor/corpus,
golden values and historical manifests were not changed. The original
[WAMR baseline manifest](../../donors/forge-wamr-baseline-v1.json) remains
historical evidence, not a live source-file existence promise.

## Retirement Boundary

The five files formed a closed, unregistered legacy source/export chain. The
current AOT graph uses `wamr_compile_fixture` and public `optimizing::executor`;
the current installed-package graph uses `verify_installed.cmake`. Old raw
`artifact.entry()`, native context/epoch ABI fields, ELF relocation details and
ABI 2/codegen 3 exports are not made operational by renaming their module import.
No raw-ABI adapter, second engine or hidden test skip replaces them.

## Open Operational Oracles

The 47 current AOT CTests cover important depth/stack, trap, cancellation,
preparation, ownership, session and provenance behavior, but do not establish
the following complete behavioral matrices:

- Integer families, boundary operands and CFG result merges, including dense
  `br_table` targets and the default branch.
- All 14 numeric memory loads and nine stores, sign/zero extension, unaligned
  access, offsets and raw f32/f64 bit transport; memory/global/growth behavior
  from the old matrix must receive an explicit current test disposition.
- All 62 FP comparison/arithmetic/conversion/reinterpret operations and their
  argument/local/generated-call raw transport. Arithmetic NaNs follow the
  approved canonical WAMR profile; reinterpret payloads and signed zero retain
  their exact bit contracts.
- Both checked benchmark workloads and their single host effect: integer mix
  checksum `15065454813286283065`, memory sweep checksum
  `11713105455123543422`. Their original argument/workload factories are
  preserved in the compiler preimage, not reconstructed from the checksums.

These gaps require focused tests through the existing compiler/artifact/executor
APIs before claiming corresponding acceptance. Removing the unregistered old
sources neither waives these obligations nor transfers historical results to
the current engine. Source review, test implementation and actual runtime
qualification remain separate gates.
