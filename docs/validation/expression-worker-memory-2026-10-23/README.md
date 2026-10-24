# Native CEL worker acceptance

Current-host macOS 26.1 ARM64 acceptance passed. This record covers the isolated
native allocation pool and the genuine installed package. It does not cover
Linux, older macOS, SDK/WASM/WAMR, Spine, Turbine or external providers.

- `acceptance.json`: gate results and allocation-pool scope.
- `focused-ctest-final.*`, `worker-oom-evidence.log`: 17 public cases and five
  direct worker cases, including real CEL allocation refusal and recovery.
- `arithmetic-oracle-final.log`: 1460 independent Fraction cases.
- `installed-result.json`, `installed-commands.jsonl`, `installed-*.log`:
  genuine install, relocation and sandboxed consumer build/run. Denial probes
  use existing producer/source/Bazel files; the consumer links only expression.
- `worker-manifest.json`, `runtime-manifest.json`, `build-manifest.json`:
  linked worker, private runtime and upstream dependency identities.
- `design-final-review.json`, `upstream-final-review.json`: independent review
  of source ownership, lifecycle, allocation imports and installed evidence.
- `raw.tar.gz`: raw build/dependency/tool outputs and both link maps, retained
  before generated builds were cleaned. It contains no installed binaries or
  producer build objects.
- `artifact-manifest.json`: SHA-256 for every retained file except itself.

The relocated helper exactly matches the native OOM-tested helper:
`de3a3cc989bcf3bb34aa17422e365fdda642941d79eb9b3835c316b447a1e43e`.
Producer paths in diagnostic `source_location` strings are distinct from build
inputs; actual consumer input objects have no producer or private CEL dependency.

Reproduction and scope are described in the
[qualification report](../../donors/expression-worker-memory-2026-10-23.md),
[installed fixture](../../../tests/package_expression_component/README.md) and
[expression README](../../../libraries/expression/README.md).
