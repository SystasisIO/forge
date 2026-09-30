# P2P shutdown: unresolved downstream sync wait

Recorded 2026-09-29. Status: **OPEN; recorded for the libp2p stabilization team**.
The original checkpoint-remediation task documented this incident only and made
no P2P, API transport, scheduler cancellation or downstream network shutdown
changes. The delivery separation recorded below removes pending P2P experiments
from the broader Wallet/signer integration tree; it does not resolve the incident.

## Observed boundary, not a proven libp2p root cause

The downstream Spine test `spine_validator_weak_vote_reorg_tests` sometimes
does not finish shutdown. After a separate local notification-lifetime repair,
146 consecutive repetitions passed before a 240-second CTest timeout. A later
instrumented run reached the following last shutdown observations:

```text
TEMP close <instance> inbound drained
TEMP close <instance> sync handle=6
... CTest Timeout 240.02 sec
```

The wait is in the downstream chain-network plugin, awaiting a sync task handle.
That task uses the P2P-backed Chain API. This places the investigation on the
sync/transport/cancellation path; it does **not** prove a defect in libp2p itself.
The unresolved await may belong to product sync, API dispatch, a storage operation,
the transport or the shared runtime. Do not attribute it to signer or Savanna
checkpoint retention without a reproducer establishing that connection.

## Baselines and evidence

- Forge worktree `forge-signer-wallet-v1`, branch `signer-wallet-lifecycle-v1`,
  HEAD `9ccc98f276897e4be3f497f3800c2ff0f8235eea`, with uncommitted changes.
- Spine worktree `spine-signer-wallet-v1`, branch
  `codex/signer-wallet-integration-v1`, HEAD
  `95f52923d21a72e73781e8d0433536f77a9c8ed7`, with uncommitted changes.
- The downstream checkout uses a dirty copy of the Forge integration, not a
  delivered exact dependency pin. These HEADs alone do not reproduce the incident;
  retain the corresponding dirty snapshots when comparing trees.
- Original local logs in the Spine worktree (not published as raw artifacts):
  - `artifacts/signer/ctest-weak-notification-lifetime-v1.log`, SHA-256
    `2b6cb98c3257743c4436d98ebf553498a739425aaa214af19f55a923db57ba15`;
  - `artifacts/signer/ctest-weak-probe-v14.log`, SHA-256
    `19c2edee5eaee0c02e4b0aaca7d6c5070b6b634587682c9686a88d41b1b251df`.

Relevant downstream code: `plugins/chain/network/plugin_impl.cpp`, its close path
and sync-task `handle->wait()`. Existing `TEMP` probes are diagnostic only and
must not be treated as release-ready code. They were not added or removed by
the checkpoint fix.

## Delivery separation, 2026-09-30

Pending P2P stabilization work is excluded from the Forge Wallet/signer delivery.
The following four files were restored byte-for-byte to the recorded Forge HEAD:

- `libraries/net/p2p/node_impl_lifecycle.cpp`: remove early
  `pubsub_value.connection_gates.close()` from `request_lifecycle_stop()`.
- `libraries/net/p2p/node_impl_pubsub_outbound.cpp`: remove the added closed-join
  rejection in `ensure_pubsub_direct_session()`.
- `tests/quic_p2p/node_session_tests.cpp`: remove the corresponding
  `pubsub_stop_completes_connection_before_tracked_task_entry` fixture and case.
- `tests/quic_p2p/session_teardown_tests.cpp`: exclude the pending strand-based
  timer/cancellation test-harness correction. The original test-harness race risk
  returns with this rollback; no correction or shutdown-acceptance claim is made.

The excluded source patch and exact preimages are preserved in the local delivery
evidence bundle, under `excluded-p2p/`. Its forward restoration patch
`restore-experiments.patch` has SHA-256
`eef8c9347d223ec1a7744bbace9d2de8c367d87309bf3285b148b9dacc250b7d`.
It restores the experiments onto the separated tree for libp2p investigation;
it is not part of the delivery candidate.

Independent Asio corrections remain in the Wallet delivery:

- `libraries/asio/task.cpp` and its task regressions release canceled queued
  capacity before notifying completion waiters. Wallet idle-expiry replacement
  depends on this behavior with a bounded scheduler queue.
- `libraries/asio/notification_waiter.cpp` and notification/affine regressions
  retain ownership through async-wait initiation and mutex unlock when another
  worker completes early. This fixes notification lifetime for shared runtime
  consumers, including Wallet compute/gate operations; the affine regression
  exercises another consumer of the same primitive. It is not a P2P shutdown fix.

The approved HTTP/P2P plugin relocation to `plugins/net`, its configuration and
identity migration, Wallet mTLS publication requirements, signer tests and
Savanna checkpoint-retention repair remain. Existing P2P lifecycle behavior,
canonical serialization and typed errors are retained. No downstream dependency
pin, transport workaround or new shutdown implementation is introduced here.

## Reproduction and ownership

From the recorded Spine worktree, using its configured LLVM/CMake/Ninja build:

```sh
cmake --build build/signer-debug --target spine_validator_weak_vote_reorg_tests -j 4
ctest --test-dir build/signer-debug -R '^spine_validator_weak_vote_reorg_tests$' --repeat until-fail:200 --output-on-failure
```

This is an intermittent reproducer, not a deterministic minimal test. Build the
target before CTest and capture the exact source snapshot, toolchain and result.
One successful run does not close the incident. No new stress run is claimed by
this handoff.

The libp2p team should identify the unfinished await, trace cancellation and
completion ownership across the product/API/transport boundary, then extract a
minimal local regression at the owning component. Compare the baseline and
integration trees before classifying it as a regression. A product-owned cause
should be returned with that evidence, not silently folded into transport code.

Acceptance requires deterministic cancellation/close coverage and repeated
downstream shutdown on the exact repaired tree. Increasing the timeout,
detaching unfinished work or bypassing its join is not a fix. This incident
remains a separate product acceptance blocker; the Savanna checkpoint repair
neither closes it nor depends on modifying it. CI and releases are not authorized.
