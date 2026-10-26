# JOB-5 single quotecharge call — source equivalence review R1

2026-10-25. **Proposed simplification APPROVED for source implementation**: remove only the first cross-WASM `gettariff` call and duplicate conditions in jobs.validate_terms (`jobs.cpp:262–268` before edit). Keep all preceding jobs guards and the existing exact `quote.price == terms.price && quote.payout == terms.payout` comparison. This is not runtime acceptance of the yet-to-be-rebuilt source; the earlier final review qualifies its recorded earlier source/binary identities.

No reachable valid-state validation is lost:

| Removed comparison / guard | Remaining owner and check |
| --- | --- |
| tariff reference service/version/hash | `bill.cpp:484–489` quotecharge still invokes the same gettariff. `:459–468` validates storage, existence of service, input schema, exact versions table key and exact reference terms_hash. Tariff.version is the immutable primary key (`bill.cppm:115–130`); publishver `bill.cpp:408–440` sets it before emplace and rejects duplicate versions. |
| tariff.asset == maximum extended_symbol | `admit_charge :276–278` checks full maximum symbol/precision/contract equals saved.asset; returned price uses the same saved.asset `:312`, and jobs retains exact quote.price equality plus its earlier price/maximum asset guard. |
| tariff.recipient == terms.recipient | quotecharge receives terms.recipient; `admit_charge :273–275` requires equality to saved.recipient and valid existing non-self recipient. Earlier jobs-specific recipient restrictions remain. |
| tariff.payout == terms.payout | `admit_charge :268–272` validates mode/schedule/current epoch; returns the complete saved.payout at `:313`; jobs still compares the entire payout (mode, anchor, period), not just mode. |
| price/admission | Existing single quotecharge retains canonical inputs, CEL evaluation, price/max, enabled bill token, positive recipient account and payout capacity checks. |

The change removes one read-only cross-WASM call plus serialization/return of the full tariff/CEL program. The old first gettariff did not execute CEL; do not claim a removed second CEL evaluation. Both calls are read-only in the same transaction, so no intervening mutable state change is part of the valid execution model.

Agreed requalification: inspect exact resulting diff and new jobs.cpp hash; rebuild integrated and standalone turbine.jobs and the native product runner; rerun the two product/ABI CTest gates and recheck direct/embedded wasm/abi/contract manifest equality. Bill/wire source and algorithms remain unchanged, so no additional bill/wire runtime repetition is needed unless the actual diff broadens. Preserve previous PASS artifacts with their original identities.

Production/commercial admission and all critical limitations in `review-jobs5-final.md` remain unchanged.
