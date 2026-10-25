# JOB-5 independent final review — R2

2026-10-25. **GO for JOB-5 local engineering settlement qualification.** No unresolved source-security blocker was found in the reviewed implementation or complete frozen product driver. **Production/commercial admission remains OFF.** The final wire refresh also passed and is included below; the selected local engineering validation gates are complete.

Reviewer performed source/artifact inspection only, no build or test rerun, no production edits. This is a scoped test-host disposition, not financial/live-chain production approval.

## Source reviewed and identity

- Initial executable guest/mirror source was reviewed under `jobs5-guest-source.json` (`review-jobs5-guest-source-r1.md`), manifest SHA-256 `efa8249ce129cee4c51507f8a1e41bb38e3d9f38f37fc02ec46cc9c59596d752`. Two final .cpp deltas below supersede those two old file hashes; all other executable guest/mirror files are unchanged. README corrections are documentation-only.
- All 15 entries of `jobs5-tests-source.json` matched at the initial source/runtime inspection (manifest SHA-256 at inspection `cc9864a862f442d43c33e88a52ee6bbdf6f4b8e8dba033647b206a40cdb5f0b5`). Frozen `settlement_tests.hxx` SHA-256 `a99d8f3c50c2593b3b6205cbd77c238f2ba764b4ef18f755d23155c196f1f325`; `contract_tests.cpp` `12e1d34e90adfed5ce6932505e365109499b274def96d0f840b2a8b047323e14`. Full source assessment: `review-jobs5-test-driver-r1.md`.
- All 32 entries of `forge-candidate-source.json` match current files. Manifest SHA-256 `6ab29d0bfb42f35ef0ec29873a7d6c943a15761b43b5ac3f2f063514b8b07f66`. Common host implementation and six new e2e cases retain the exact reviewed hashes in `review-jobs5-host-source-r1.md`.
- Forge base `6a34cb8eac3b4545e3fa0ef745f0381e54889daa` plus reviewed pending tracked patch/untracked source hashes is explicitly recorded in `sdk-source.json`. It does not claim a clean historical SDK release. The recorded candidate source-manifest and installed foundation hashes match independently; all 11 installed archive hashes match foundation.json.
- Turbine base `baaa45a36df53f2366ac77c529d80cf1700fd80d`; these reviewed changes are pending source, not a published/pinned release.

## Final two-file source delta (independently verified)

- Current `guest/contracts/turbine.jobs/jobs.cpp` SHA-256: `47df2b50a17adca0173ee0ec2be300106667d99d32f36cf17bdbbfc4253e8260`.
- Current `guest/contracts/spine.bill/bill.cpp` SHA-256: `d1cd2224ba94ac9d68ec74f3c61cfa35581bd6aaf0be0d511cc69a9c1a2329d9`.
- Reviewer reconstructed both original frozen files **in memory** from base `baaa45a…` and `jobs5-guest-from-base.patch`, verified reconstructed hashes equal the original manifest, and diffed current source. Exactly two changes: jobs removes the seven-line duplicate cross-WASM gettariff/comparison at former lines 262–268; bill::gettariff now checks both `value.version == reference.version` and existing exact `terms_hash` at lines 467–468. No other change in either file.
- All jobs-specific identity/profile/deadline/amount/token guards and exact returned quote.price/full payout comparison remain. quotecharge still calls the same owning gettariff, so storage/service/schema/version/hash checks survive. Full recipient/asset/mode/price/payout/admission checks remain in the owning bill helper. The explicit row-version guard also preserves the old defensive check if persisted row content is inconsistent with its database primary key.
- This removes one read-only call and full tariff/program return, not a second CEL evaluation. Source equivalence PASS; product and bill runtime were rerun against the changed binaries below. Earlier R1 XML/logs remain retained with their original identities.

## Independently parsed retained runtime evidence

| Artifact | Observed result | SHA-256 |
| --- | --- | --- |
| `job5-contract-tests-r2.xml` | 2/2; product actual-WASM acceptance + ABI, no failures/errors/skips; 78.922 s command elapsed | `9b9aecf308a25f50841ba081670205b93a64e13f6c439fa8b28d9104676ae7ac` |
| `job5-bill-tests-r2.xml` | 6/6; contract, transaction builder, ABI, positive/negative provenance, independent goldens; no failures/errors/skips; 6.127 s command elapsed | `5f3104289f062c6bb47201abceef182f79986f13c13622c6a78a02b0dddb965f` |
| `job5-forge-e2e-tests.xml` | 1/1 CTest; 59 Boost cases including common transaction cases, no failures/skips | `b63854d8f311596258cfe812f0016d22712ba0d94ccc7225f81c2a1f922bc3db` |
| `job5-forge-sdk-tests.xml` | 2/2; runtime archive + package prefix/tampering, no failures/errors/skips | `7261e6a3fba656d1cef27600703c150a6fd4681a2aff25e898653d4362e8fc82` |
| `job5-wire-tests.xml` | 3/3; native/actual WASM, actual ABI, fixed goldens, no failures/errors/skips; 0.918 s command elapsed | `0846ec3a445bb3aaaab93cbf22e718d1fd1dc5f33526fdd98e4c00b93dd6dbb8` |

The full product `LastTest.log`, not truncated XML stdout, shows **27 product PASS groups: 18 prior groups and all nine JOB-5 groups**. It also shows additive quotecharge arguments/result and exact payout/settlement ABI checks passing. The frozen test source manifest's `old_product_groups=17` is a bookkeeping undercount, reported to the coordinator; no missing execution is inferred from it. Current R2 full logs are retained as `review-jobs5-contract-LastTest-r2.log` (SHA-256 `72d74f659c6351e819a7b35d9b1f6b5c269a40d4fd8a7fb7a5e1fdfcb4cf5dda`) and `review-jobs5-bill-LastTest-r2.log` (`6ceb2d212b6127bea3bc276d6418bcf05623978e166ac08e3b756e0f8b4c7d51`). Earlier reviewer R1 full logs and original XML files remain separate historical PASS evidence.

Build/configure JSON records inspected show exit code 0 for product guest/native and standalone guest; native testing uses the current Forge build tree, guest uses the freshly installed Contract SDK. This is not a fresh installed native Forge consumer qualification.

Final wire full log independently inspected: accepted=219/rejected=57 transport/crypto cases, native/real-WASM independent Keccak vectors and exact invalid-witness-vs-proven-fault distinction, actual natural Abigen arguments/records, unchanged fixed Raw/SHA256/K1/enum goldens. Retained as `review-jobs5-wire-LastTest.log` SHA-256 `10e52510fa1d33786bced39e8d282deda5963ed42b23923ec0abcae81c53c385`. Executed jobswire.wasm is 171673 bytes, SHA-256 `061bb64764fea75aa336b2ad9834055c74062ba15a40c5f782c49b289e538e84`. The local matrix has 16 unique selected CTest gates when the two retained actual-MPC/crypto JOB-4 tests are included; this total is not 16 live-chain/financial scenarios.

## Direct and embedded equality independently checked

| Artifact | Bytes / SHA-256 | Comparison |
| --- | --- | --- |
| turbine.jobs.wasm | 293552 / `77f25fc019c0436ba6eeb58cb4cb500ba2c36f5c60cc5d79b22a365828f06012` | standalone == jobs fixture |
| turbine.jobs.abi | 32545 / `5c99abb636f1d720869b5e9ab85d8060c4dcd03bc659bce541225cbd3f1c581f` | standalone == jobs fixture |
| turbine.jobs.contract.json | 3867 / `53507d41bc05dec790d713e5c615eb983b7b9724e2a90299626c153b809618d5` | standalone == jobs fixture |
| spine.bill.wasm | 179233 / `c17e6ecf7e3b7d21a98ace6436ee21e2a92e4957d172108a2434730fc77f3b60` | jobs fixture == bill fixture |
| spine.bill.abi | `452cc8f5c94ee54b1761270de8961995ddcc8ef18b4d33ba630785f708f8e3ec` | jobs fixture == bill fixture |
| spine.bill.contract.json | 3903 / `e1595f93992f577dd502488b75a6c9bbde0b436fee6a6681cdc8e5c23c733e53` | jobs fixture == bill fixture |

Executed jobsreader.wasm SHA-256 `d454c0c20b5ce7e55eac2e7e7f728e54269bae193f9b10a39fd0c4e312ac04ea`; jobstoken.wasm `8f540eefc151c8cf98931d8fa428f56fa8031ac2eb5308993dd3478ab1571b9b`; R2 native product runner `be351bdf967075a1d7cea77b260b32c03863db3d6e8db61396ea1b829f66bab6`.

Current equality record `job5-guest-equality-r2.json` SHA-256 `e27beab17080ecf9ea30647dc4943f56c823268c9c55e5a83de1d9bcdafd20ba`: reviewer independently compared all six pairs and verified each final contract.json embeds the actual current WASM hash.

Intermediate manifest mismatch is retained in `manifest-equality-failure-before-refresh.json` (SHA-256 `35ba145752c68276a8a0d080686a86da5de4d5a423dd4fc3ad6ce0ed91360a31`). It recorded stale manifest WASM metadata after a targeted build. The subsequent complete ALL build log contains exactly two steps generating spine.bill.contract.json and turbine.jobs.contract.json, no WASM relink; exit 0 in 0.081 s. This refresh corrected stale generated metadata; the initial failure is not erased or counted as product runtime failure.

## What this acceptance establishes

The fixture explicitly executes the complete bounded real-WASM jobs→bill→token→notifications→finish recipe in one real ObjectDB transaction. Production bill owns CEL price/admission/hash/funding/payout. Exact child bytes and permissions, ordered notifications and all remaining effects are checked before commit. Positive/zero balances, sequence/current epoch/deadline, exact callback/full asset, reentry, caught-child poison, real late token/notification/finish failures, abort after successful finish against an existing bucket, FIFO retirement and subsequent claim are covered. Protocol rows/indexes/table ownership, token balances and complete oracle effects roll back; monotonic internal ObjectDB allocation seals are explicitly outside byte-identical rollback claims. Native oracle assertions have a different exception type from guest rejections and cannot be swallowed as expected negative tests.

## Critical limitations remain open

1. **Post-reveal liveness:** quotecharge checks current bill admission at open/settle. It does not reserve future token policy, recipient account, code/permissions, RAM/CEL/resource/payout capacity. A late change can block settlement after the key is public. Revealed reserve remains for retry; an ordinary buyer refund cannot safely repair that state. A continuity/recovery policy is mandatory before commercial use.
2. **Chain execution/finality:** sender and permissions are trusted fixture inputs; no live account authority/linkauth/code graph, automatic chain scheduling, consensus/forks/finality or live funds were tested. Local common rollback is real for explicitly invoked children but does not establish Spine transaction-tree semantics. A rejected reveal may still expose its public key; bounded inclusion/finality assurance remains open.
3. **Cryptographic scope/trust:** JOB-4 stays restricted to direct raw HTTP body <=32 KiB, <=16 blocks and trusted buyer MPC verification. Authenticate the complete verifier result including visible request/header bytes plus independently expected context/current nonce before authorization; signing opaque retained evidence/root is insufficient. Use a fresh one-time key per offer and durable complete ciphertext before arm. Public disputes reveal the challenged block; leaked ciphertext plus the public key exposes the rest. No general HTTP/JSON/Labs predicate or independently offline TLS proof is claimed.
4. **Remaining product gates:** two TurbineOS, durable outbox/restart recovery, WAN/full worst-case resources/cost, production privacy/availability, Linux and real Labs remain NOT_RUN. This review authorizes no deployment, merge, payment or commercial activation.

Material documentation trust contradictions from `review-jobs45-docs-r1.md` were corrected on reinspection (digest vs metadata, full verifier result, profile-2 registration and separate commercial gate). Final status/evidence wording is still the coordinator's publication step and must retain these scope limits.
