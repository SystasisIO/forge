# CEL artifact and Contract SDK acceptance

The final macOS ARM64 qualification passed for Forge `e815a6f9` and Spine
`7a6db0ae`. These are implementation revisions; later documentation commits do
not change the tested sources. No Linux, running Spine node, external provider
or token settlement was tested.

- [acceptance.json](acceptance.json): revisions, gates, toolchain, limits and
  SHA-256 of the actual installed helper, SDK foundation and both guest modules.
- [native-final-ctest.log](native-final-ctest.log): final native/runtime/worker,
  oracle, SDK bridge and existing test-host suites, 5/5 PASS.
- [backend-tests-r1.json](backend-tests-r1.json): the two upstream-backed private
  arithmetic and artifact mutation tests, both PASS.
- [installed-result.json](installed-result.json): genuine installation,
  relocation, producer/source/old-prefix/Bazel denial and a new worker loading
  the saved artifact. No producer archive or BMI is used as a guest archive.
- `sdk-*.json` and [surface log](sdk-surface-fixtures.log): fresh developer SDK
  build/install and installed registry/surface fixtures. The baseline 200
  imports remain unchanged; the expression capability adds three optional ones.
- [tariffs-final-ctest.log](tariffs-final-ctest.log) and
  [tariff-qualification.json](tariff-qualification.json): actual SDK-built
  `tariffs` and `tariffclient` guest modules through the installed Forge WAMR
  test host, 4/4 PASS. Runtime guards verify the recorded input hashes.
- [manifest.json](manifest.json): checksums of retained receipts. Command paths
  use `$FORGE`, `$SPINE` and `$QUALIFICATION` for the corresponding source and
  local qualification roots; they are descriptive placeholders.

[Cleanup receipt](cleanup-result.json) records removal of five task build
directories (5.85 GiB allocated), after stopping the task Bazel server and
checking for open files. Foreign builds were not removed.

Raw build logs, worker/backend/runtime manifests, installed native and guest
SDK packages, and generated WASM/ABI/manifests were retained locally before
cleaning the task build directories. The SDK reused a matching base sysroot;
its shared foundation sources and guest archives were rebuilt from this Forge
revision. The global Spine SDK pin and its previous 16 contracts were unchanged.

The worker limit is a 64 MiB allocation pool, not an RSS or address-space limit.
The private worker protocol is version 2; the persisted artifact format,
restricted profile and arithmetic semantics are version 1. Production Spine
integration must register these optional intrinsics and account for their
native work before enabling tariff execution.

See the [qualification report](../../donors/expression-contract-sdk-2026-10-23.md)
and [expression API](../../../libraries/expression/README.md).
