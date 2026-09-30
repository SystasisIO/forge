# Wallet / signer implementation status

Forge scope: **LOCALLY ACCEPTED FOR DEV**. Downstream Spine acceptance remains
pending. No CI or release was run. Local acceptance does not establish downstream
product or live readiness.

Approved work is the Forge Wallet library/plugin and optional block execution
hook, followed by downstream Spine signing storage/service, signer integration,
`spinekd`, and program renames `spinectl` / `spinead`. Forge owns no signing DB.

## Isolated baselines

- Forge branch `signer-wallet-lifecycle-v1`, base
  `9ccc98f276897e4be3f497f3800c2ff0f8235eea` (refreshed origin/dev).
- Spine branch `codex/signer-wallet-integration-v1`, base
  `95f52923d21a72e73781e8d0433536f77a9c8ed7` (refreshed origin/dev).
- At the prior integration snapshot, existing checkout contents were preserved
  and the isolated Spine checkout used the Forge baseline plus the reviewed,
  uncommitted patch for local integration. Delivered dependency commits are
  recorded separately in Git history.
- Prior v5 implementation: main agent and independent GPT-6 Sol xHigh reviewer.
  Current delivery team: coordinator, GPT-6.1 Sol High developers for scoped
  separation/formatting and validation, and a GPT-6.1 Sol xHigh independent
  reviewer of immutable candidate artifacts.

## Slices

1. Block execution hook: implemented and locally verified, including revocation
   of retained lazy operations, current key checks and cached-result verification.
2. Keystore ownership: implemented, with private-parent migration documented and
   fork/lock/atomic replacement/installed-consumer checks passed. Format unchanged.
3. Wallet protocol/API/runtime: implemented. Review findings around cancellation,
   idle refresh, scheduler admission and canceled-timer capacity are fixed and
   covered by regressions. Queued scheduler cancellation now releases capacity
   before waking completion waiters. Lock/open/fault revoke the idle timer.
4. Prior Forge integrated v5 source review: **no reportable findings**, GPT-6 Sol xHigh.
   Immutable implementation patch SHA-256:
   `2f031fccbaf1095dbca17767b2e559ebd2ec920366f8345edb5106fa69c2d59c`.
5. Spine guard, composition, program renames and packaging: downstream work in
   progress, not accepted. Full product/live Docker acceptance and delivery: NOT RUN.
6. Current Forge candidate combines Wallet/signer, the approved HTTP/P2P plugin
   relocation to `plugins/net`, and Savanna checkpoint-retention remediation.
   The exact downstream Forge pin and full Spine acceptance are separate
   requirements recorded in dependency history and downstream acceptance evidence.

## Local validation

The separate downstream [P2P shutdown incident](p2p-shutdown-sync-handoff.md)
remains open under libp2p stabilization. Delivery separation restores four files
to baseline `9ccc98f276897e4be3f497f3800c2ff0f8235eea`:
`node_impl_lifecycle.cpp`, `node_impl_pubsub_outbound.cpp` under
`libraries/net/p2p`, and `node_session_tests.cpp`, `session_teardown_tests.cpp`
under `tests/quic_p2p`. Their exact excluded patch/preimages are preserved outside
the delivery candidate. The original teardown-test race risk returns; no fix or
shutdown-acceptance claim is made.

Independent Asio changes remain: queued cancellation releases bounded scheduler
capacity before completion notification for Wallet idle-timer replacement;
notification initiation retains ownership through mutex unlock for early
completion across workers. Wallet uses the existing compute executor and gate;
notification/affine regressions cover the shared primitive. These changes do not
close the P2P incident. Wallet mTLS publication, canonical serialization, typed
errors and existing P2P lifecycle behavior remain.

Configure: CMake/Ninja, Homebrew LLVM 22.1.8, macOS SDK, BUILD_TESTING=ON,
FORGE_ENABLE_MODULES=ON. Build directory `build/wallet-debug`.

Prior implementation v5 evidence (historical; not the current combined-candidate
acceptance). Targets were built first with `-j 4`:

- `ctest-wallet-v9.log`: 6/6 PASS, 174.72 seconds: Chain Signer, Wallet, Wallet
  fsync-fault injection, Asio, real TLS/mTLS, real QUIC/P2P.
- `ctest-regressions-v5.log`: 10/10 PASS, 45.39 seconds: Structure, Raw, Crypto
  Signer, Keystore, HTTP server plugin, App, Schema, API Core, API Transport,
  HTTP/WebSocket.
- `packages-manual-v4.log`: five installed consumers configured, built with
  `-j 4` and executed: Keystore, Wallet library, Wallet plugin, Chain API and
  Chain Signer. Direct module dependencies were corrected where these tests
  exposed omissions. No package failure is recorded as PASS.

The prior reviewer verified these logs and scoped approval to Forge only. They do
not establish current-candidate acceptance, Spine integration, production
deployment or live Docker readiness.
Temporary local evidence is in
`artifacts/local-wallet/` (not intended for publication with raw generated logs).

## Current combined-candidate gates, 2026-09-30

Local gates refer to the frozen combined runtime/source candidate v2:

- Tested v2 patch SHA-256:
  `663c93b8a8217a2d9b990e27fb652913d3e83014fdd2b037b32e2c967a808c3f`.
- Reconfigured candidate build in the existing build directory (`-j 4`): PASS,
  48.24 seconds.
- Scoped formatting: PASS, 74 changed paths after normalizing 124 relocation
  paths. Existing baseline whitespace was preserved.
- Full repository clang-format diagnostics still include baseline/donor
  failures; the scoped PASS does not establish repository-wide formatting PASS.
- Structure and net-plugin migration checks: PASS within the combined CTest matrix.
- Combined Wallet/signer/runtime/transport/checkpoint CTest matrix: PASS, 26/26,
  1219.73 seconds.
- Installed consumers from a fresh install prefix: PASS, 12/12 configured,
  built with `-j 4` and executed. Coverage includes Keystore, Wallet library/plugin,
  Chain API, Chain Signer, Savanna, HTTP server, P2P node/resolver, descriptor
  surfaces, plugin composition and Asio.
- Independent GPT-6.1 Sol xHigh review of the immutable v2 candidate: no
  reportable findings. The final documentation-only amendment is reviewed
  separately from the tested runtime/source candidate.

The candidate excludes P2P stabilization experiments. Downstream P2P shutdown
and full Spine product/live Docker acceptance remain open; no new live acceptance
is claimed. The exact delivered dependency pin is a separate requirement recorded
in dependency history. CI is NOT RUN.

Downstream integration must pin the exact published Forge dev commit and rerun
Spine focused, full and live acceptance on that dependency. Record the delivered
commit in dependency history, not as a changing claim in this report. Required
CI and release gates remain separate; never dispatch CI or bypass protection.
