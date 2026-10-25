# JOB-5 SDK package follow-up review R1

Disposition: **PASS for the reviewed packaging delta**. No source finding. This supplements JOB-4 engineering GO; it does not extend the cryptographic or deployment claims. Reviewer performed read-only source/artifact inspection, no build/test rerun.

## Source and mechanism

Forge worktree: `worktrees/forge-tlsnotary-jobs3-v1`, base `6a34cb8eac3b4545e3fa0ef745f0381e54889daa` (upstream documentation-only advance).

- `guest/cmake/generate_foundation_manifest.py:9–20,30–43` includes `libforge_guest_crypto_exchange.a` in the existing fixed foundation archive list; generation fails if the file is absent and records its full SHA-256. SHA-256: `ff31f1d8516d12f33538595c1e337ea12e8cc5d2354813bc21ef20125e1538cf`.
- `guest/tests/tooling/check_package_prefix.cmake:74–87,126–145,241–265` adds the same archive to synthetic prefix creation, its manifest, and the tamper loop. Each tamper case requires configure failure AND the checksum diagnostic, then resets that archive before the next case. This prevents an earlier damaged archive from causing a false-positive rejection for the exchange archive. SHA-256: `b29422fdf027211a2f9f4b08b2fabb77c7c97280461ecc4900181272e8b69113`.
- Existing `guest/cmake/ForgeContractConfig.cmake.in:73–109` requires the manifest, checks version/existence, hashes every listed archive, and fails package discovery on mismatch. Its unchanged SHA-256 is `17121b8e61d0c099a6647ba441c9b94ef2dcefcc088029a58fe946b9ff459f3d`.

The delta reuses the canonical package integrity mechanism and introduces no second dependency graph or cryptographic primitive. These hashes detect inconsistent archive/manifest contents within a trusted SDK package; they do not authenticate a maliciously replaced manifest and all package files.

## Independently inspected retained runtime evidence

- `job5-forge-sdk-tests.xml`: 2/2 tests, zero failures/disabled/skipped. Tests are `test_forge_contract_sdk_guest_runtime_archive` and `test_forge_contract_sdk_package_prefix`. Parsed with `/usr/bin/python3` ElementTree. SHA-256: `7261e6a3fba656d1cef27600703c150a6fd4681a2aff25e898653d4362e8fc82`.
- `sdk-build-manifest-r3.json` and `sdk-install-r3.json` both record exit code 0. SHA-256 respectively `1521331fdacecc501212be70fe413407d90503f1275cfc172fce46611d8d8463` and `bd64f87a79429df9be14e306935c852cbf19ae042d5cbbd04ab41913c2164ad4`.
- Installed `sdk-prefix/share/forge-contract/foundation.json` now lists 11 archives including exchange. Its exchange digest is `d422422002899367c86bf5de9a0823d5b90caf89f3794b6b29d4e523357604d5`.
- Independently hashed installed `sdk-prefix/sysroot/lib/libforge_guest_crypto_exchange.a`: 6882 bytes, exact matching digest. It is byte-identical to `build/sdk/sdk-sysroot/lib/libforge_guest_crypto_exchange.a`.

Final consumer reconfiguration/build identities remain the coordinator's separate JOB-5 qualification step. This review does not infer final consumer success from package-prefix tests.
