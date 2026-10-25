# JOB-4/5 architecture documentation review R1

Read-only review on 2026-10-25. No document or production-source edits. The new JOB-4 sections correctly keep production/commercial admission OFF, describe the restricted direct-response profile, and require current-request policy plus authentication of the complete verifier result. The following earlier passages should be reconciled before publication.

## Material trust/format contradictions

1. **Hash vs authenticated metadata:** `docs/architecture/turbine-jobs-evidence.md:139–141` says the hash commitment binds direction/ranges/algorithm. This is ambiguous at the security boundary and conflicts with the correct `:168–170`. Say explicitly that the digest is only `SHA256(plaintext || blinder16)`; direction/ranges/origin/lengths are fields of the authenticated verifier record, compared against independently expected policy/context. A matching digest alone authenticates none of that metadata.
2. **Retained record is narrower than the whole verifier result:** evidence `:164–170` still reads as if signing retained evidence plus context suffices for the full source operation. New `:255–263` correctly says retained digest excludes visible sent/received bytes. Add that distinction to the earlier paragraph: retaining/signing evidence is enough for that narrow commitment/opening record; authorizing JOB-4 requires the full authentic verifier result, including visible request/header bytes, independently expected context and the current nonce/freshness policy.
3. **Registered profile contradiction:** `docs/architecture/turbine-jobs-contract.md:361–362` says all other profiles are rejected and crypto-profile id/payload are unallocated. This contradicts accepted profile `id=2/version=1` at `:515–533`. Replace with the current allowlist: synthetic 1/v1 and restricted direct-source 2/v1 only in test deployment/admission; all others rejected.

## Stale status and qualification wording

4. **JOB-4 is still repeatedly future work:** evidence `:3,7,27,101,137,173–175,219–222`; architecture `turbine-jobs.md:46–50`; plan `2026-10-24-turbine-jobs.md:86–87`. Identify the general/commercial profile as research while marking restricted 2/v1 as implemented and engineering-accepted. Replace “До JOB-4” commercial gate with “До отдельного коммерческого допуска”; completing JOB-4 alone must not look sufficient for activation. Keep the generic challenge table at evidence `:187–198` explicitly future/general and link the existing kind=1 wire.
5. **JOB-5 admission and rollback are now partly implemented:** contract `:424–427,435–442` still requires future shared rollback model and future bill admission check, whereas `:539–555` describes the implemented `transact` and shared `quotecharge`. State source implemented / product runtime qualification in progress. Qualify “manual calls do not prove common rollback” as calls performed in separate `invoke` transactions; grouped calls now have a common real ObjectDB transaction. Preserve the separate live Spine/authority/finality gate and unresolved post-reveal continuity policy.
6. **Historical pins look current:** contract `:10–13` attaches old JOB-0 Forge source/dev pins to current JOB-4/5 status. Label them historical JOB-0 evidence, then reference the current immutable JOB-4/5 source manifests and eventual published Forge pin. Architecture `:28–34` should likewise label its initial base as historical. Do not imply uncommitted engineering changes are supplied by an older vendor pin.
7. **Padding/visibility precision:** evidence `:244` says padded “to 32 bytes”; use “to the next multiple of 32 bytes” (maximum 2080-byte ciphertext). At `:252–253`, say that *the public dispute payload* discloses one block's plaintext/blinder; the buyer decrypts the complete bundle after key disclosure, and ciphertext leakage then exposes the rest. Plan `:96` can mention that only the final plaintext block may be shorter than 2048 bytes.
8. **Future tests must not read as completed scope:** architecture `:298–300` uses present tense for two TurbineOS, but plan `:18–19` correctly marks JOB-6 NOT_STARTED. Say “target JOB-6 scenario; not yet run”. Contract `:489–490` should limit accepted “privacy and budget” to the restricted local test construction/caps; production privacy and a complete worst-case resource/cost budget remain gates as plan `:109–110` states.

## Reviewed document identities

| File | SHA-256 |
| --- | --- |
| `docs/architecture/turbine-jobs.md` | `e7c7f80d14eca857b46f5dd1a1dec8f207b9aa676c7bd01bd463339521175430` |
| `docs/architecture/turbine-jobs-evidence.md` | `37e27fed2d3492d8e9c8c9649fae2fb5ecbcc1f9d85bdb8aa39a6ef6c976ed0f` |
| `docs/architecture/turbine-jobs-contract.md` | `b709d54b20fe49636a7bd5b88f4cbbb7b00f860384eed2ec7ced40885991f6d8` |
| `docs/plans/2026-10-24-turbine-jobs.md` | `ab84bf624a4d79df935a0b6a4d86146419f91c61f9a55fd3f1d9bcf139d260d9` |

No new source-security finding arose from this documentation pass. The earlier engineering GO remains scoped; the documentation should not be published as a single current specification until the above contradictions are reconciled.
