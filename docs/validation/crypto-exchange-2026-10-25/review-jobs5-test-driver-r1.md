# JOB-5 frozen product test driver source review R1

2026-10-25. **SOURCE PASS; no blocking finding.** Product runtime disposition remains pending the coordinator's build/test artifacts. Reviewer made no production edits and ran no builds/tests.

The two declared frozen file hashes match exactly. Reviewed the complete 843-line `settlement_tests.hxx`, its inclusion and nine calls in `contract_tests.cpp:3013,3043–3051`, plus modified fixture values, reader/token WASM sources and test CMake integration.

## Common transaction and complete explicit recipe

- `settlement_tests.hxx:186–360`: one existing Forge `host::transact` contains actual jobs.settle, actual emitted bill.charge, actual emitted fundbill, actual emitted token.transfer, the token's ordered jobs/bill notifications and actual emitted bill.finish. Queries execute the real reader WASM and nested read-only product calls, observing pending rows in the common ObjectDB transaction.
- `:226–269,299–331`: every emitted child is unpacked exactly and compared against expected target/name/authority and immutable tariff/inputs/recipient/full asset/amount/sequence/epoch/deadline/hash/memo. Production bill remains the owner of financial computation; the fixture does not emulate its payment logic.
- `:71–89,313–317,346–350`: successful completion requires exact inline count, unchanged original inline prefix, exact ordered recipients, and equality of every other oracle field after normalizing only explicitly configured sender/authority/time. Context-free/deferred actions, features, parameters and console effects cannot silently remain undelivered. Zero charges require exactly one charge and no funding/finish/token child or notification.
- Claim `:778–814` similarly verifies and executes exact token bytes and bill notification. Recipient is explicitly an account with no registered contract/code; the requested notification is a documented no-op for this fixture. This is a bounded known recipe, not a generic action router or proof of native chain scheduling.

## Nine groups and adversarial strength

1. `:371–408`: M>P and M=P, zero price with positive/zero M, exact integer balances and accumulated payout; disabling jobs' new token/profile admission preserves existing settlement.
2. `:410–434`: completion in reverse job-PK order gets consecutive payment sequences and current epoch, exact 60-second expiry, late exact replay and unknown receipt.
3. `:436–467`: required charge authority; foreign/direct sender even with bill authorization; payer/sequence/hash, amount ±1 and token contract/symbol/precision substitutions. The complete batch rejects and restores reserve/stage/counter/receipt.
4. `:469–501`: settled reentry emits no duplicate charge; duplicate funding before transfer is rejected; catching the child error still poisons the group; subsequent clean retry is exactly once.
5. `:503–556`: actual token credit overflow (after earlier debit), each notification, finish sender/hash, seeded late payout overflow, and a typed outer abort *after successful finish* all roll back. The existing-bucket/previous-success case preserves prior sequence/payment/accrual and retries the second order cleanly.
6. `:558–603`: disabling bill token or changing any jobs/bill/token code snapshot blocks settlement, preserves revealed reserve and prevents ordinary cancel/release/expiry; restoring policy allows one successful retry.
7. `:605–711`: read-only admission parity and absence of effects; recipient/maximum/precision/mode/enabled-token/missing recipient account/payout-overflow cases; sequence exhaustion; checked uint32 expiry with last representable deadline. Zero price correctly does not require a recipient token account.
8. `:713–763`: 1025 real completed zero payments exercise bill's actual FIFO window 1024, distinguish retired/retained/not-executed, and preserve permanent jobs receipt without recharging.
9. `:765–843`: accumulated claim after period closure transfers the exact accrued amount; real token credit failure restores previously erased payout/debit; successful replay emits no second payment.

## Rollback and exception oracles

`settlement_protocol_snapshot:91–140` retains all touched protocol primary rows, table presence/count/payer and 64/128/256-bit secondary presence/payer/key for the bounded fixture; it includes balances, jobs/settings/exchange/settlement, bill domains/payments/payouts and relevant next keys. `rejects_settlement_batch:143–162` compares that plus the complete oracle_state. It intentionally does not demand byte-identical ObjectDB allocator seals. Existing strict `fixture::rejects` was not weakened.

The negative tests catch only concrete `tooling::testing` child exceptions (or the explicit `settlement_abort`). Native `FORGE_ASSERT` throws `forge::exceptions::context_error` via `libraries/exceptions/exceptions.cpp:269–279`, distinct from `tooling::testing::exceptions::assertion_failure`. Therefore an earlier driver/oracle assertion is not miscounted as an expected guest rejection. The JOB-4 exact-decoder diagnostic helper at contract_tests.cpp:204–224 likewise cannot swallow its own wrong-diagnostic assertion.

Test-only row seeding remains under reader fixture code; production guest ABI does not acquire seed actions. Reader uses actual read-only contract calls, and token open/transfer use actual SDK DB operations. Fixtures remain trusted sender/permission inputs, not real authority graph validation.

## Scope and production-source continuity

All executable guest jobs/bill/mirror files still match the earlier `jobs5-guest-source.json` reviewed in `review-jobs5-guest-source-r1.md`; only the expected jobs README correction has changed since that snapshot. That prior source PASS remains valid for executable logic. The SDK provenance now explicitly identifies Forge base `6a34cb8eac3b4545e3fa0ef745f0381e54889daa` plus pending source/patch hashes in `sdk-source.json`; it does not claim a clean historical release supplies these changes.

Await actual product CTest/XML/ABI results and final native/guest/direct build identities. Even after local PASS, live permissions/linkauth, chain action scheduling, consensus/finality/forks, live funds, node restart/outbox recovery, Linux, real Labs and commercial availability/privacy/economics remain unqualified. Mutable bill/token/account/code/resources can still block post-reveal settlement; quotecharge is current admission, not future liveness assurance. Production restricted profile remains OFF.

## Inspected source identities

| File | SHA-256 |
| --- | --- |
| `tests/integration/jobs/contract/settlement_tests.hxx` | `a99d8f3c50c2593b3b6205cbd77c238f2ba764b4ef18f755d23155c196f1f325` |
| `tests/integration/jobs/contract/contract_tests.cpp` | `12e1d34e90adfed5ce6932505e365109499b274def96d0f840b2a8b047323e14` |
| `tests/integration/jobs/contract/fixture_values.hxx` | `02bfb028e41b09e65aa1b005ff00ec6b1a76189de6aff3984a0a9a19af1eb995` |
| `tests/integration/jobs/contract/guest/reader.cpp` | `d060d666c92da80d90a5d08ea12dd6d467c666e9ebbbf06f3c91a5f5d455a54f` |
| `tests/integration/jobs/contract/guest/include/jobs/reader.cppm` | `a30b2f62f21b97148598b6aa20f04f2784f2d3e9631f280960dd3ba249c00904` |
| `tests/integration/jobs/contract/guest/token.cpp` | `b9f64a6145ad7790a9b21538cda965b2057853a527adbb76faf6807f0ec43fc6` |
| `tests/integration/jobs/contract/guest/include/jobs/token.cppm` | `89cb76627d140dde4359d4e77cc7411fa08197994cd507778566c5796aaf5d83` |
| `tests/integration/jobs/contract/CMakeLists.txt` | `60b1bf96c2cb213e36a82cf8347f1e991b62ef9afd3c769033826aeb343bfc10` |
