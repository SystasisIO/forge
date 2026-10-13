# oidc-agent: Wrong Returned-Token Expiry And Audience-Only Cleanup

Status: **NOT_SUBMITTED**. Evidence: 2026-10-14. Review: **CLEAN (vendor + adapter scope)**.
Fork acceptance: **Linux native PASS (covered cycle)**; installed Forge verification: **PENDING**.
Affected: upstream **5.3.8**, [f31513acaacaa7ebc115b3a329105d449b43ac99][baseline].
The October 14 comparison found the relevant paths unchanged at
[prerel 431dcd8a930e092a47de87b1782d24ff3727cae0][prerel]; this is not a blanket
claim that current upstream is unfixed. Recheck releases/duplicates before submission.
Published [SystasisIO fork pin 28d6f89701b64d5150b05d01f85b8a8eed54808e][pin],
profile `5.3.8-systasis.1`: [token fix d9661b3e61b933a8dd2ddc04bd4727322824330d][fix],
then separate Make/pkg-config packaging in the pinned commit.

## Defects And Impact

With a loaded account, request a token by account name or issuer with nonempty
scopes and/or audience. Refresh returns a new token whose lifetime differs from
the cached default token. In `src/oidc-agent/oidc/flows/refresh.c`, `SAVE_AT` is
disabled for such overrides; [the parser][parser] updates account expiry only
when saving. Both [daemon token handlers][handlers] nevertheless reply with
`account_getTokenExpiresAt()`, pairing the new token with the default token's
expiry. Missing `expires_in` on a default refresh can also retain old expiry.

Separately, audience-only refresh returns a noncached allocated string, but the
handlers free it only when `strValid(scope)`. That result is not released.
Borrowing the account string on an ordinary default cache hit is not itself a bug;
uniform caller-owned results are the fork's chosen internal contract.

The public library's `src/api/tokens.c` only parses the daemon's expiry:
**replacing the library alone cannot fix the daemon**. Wrong metadata can cause
premature rejection or delayed renewal; the cleanup defect can retain allocations.
No resource-server bypass, issuer-policy failure, or production exhaustion is claimed.

The synthetic fixture fixes time at `1700000000`, caches a token expiring at
`1700005000`, and returns a different token with `expires_in=60`:
| Case | Expected | Baseline | Patched |
|---|---|---|---|
| Scope-only / audience-only / combined | Reply expiry `1700000060`; default cache unchanged | Cached expiry; assertion fails | PASS |
| Default refresh without lifetime | Unknown expiry 0, not old lifetime | Stale-expiry assertion fails | PASS |
| Audience-only cleanup | Free noncached result once | Cleanup assertion fails | PASS |
| Nondefault then default request | Preserve default token/expiry and cache hit | First reply has wrong expiry | PASS |

Strict malformed-expiry/missing-token rejection and uniform ownership are additional
hardening, not separate proof of upstream exploitability. 106 failed checks include
repeated shapes and stronger ownership assertions, not 106 distinct defects.

## Final Evidence And Boundaries

The identical final fixture was executed on pristine baseline and frozen patch:
| Run | Checks / Failures / Errors | Log And SHA-256 |
|---|---|---|
| Pristine RED, expected assertion failure | 131 / 106 / 0 | [RED](../validation/auth-resource-2026-10-14/linux/oidc-expiry-red-final.log); `69dcd2e08c6f67f82e7ff2ca6e9a386fc766dcd125040640dab5c838c31b9e21` |
| Frozen patch normal GREEN | 131 / 0 / 0 | [GREEN](../validation/auth-resource-2026-10-14/linux/oidc-expiry-green-final.log); `d20ec56370e3684b25b990449347c649bacd0b3f72d118ccb7ff37b5ba439903` |
| Frozen patch ASan/UBSan GREEN | 131 / 0 / 0 | [Sanitizer](../validation/auth-resource-2026-10-14/linux/oidc-expiry-asan-final.log); `5e93f5b561c7341723cbee94e15214c15a066cb5e3d1bbeaa5a5cf6599709988` |

**P2 signed-`time_t` range issue: CLOSED in source and final tests.**
Expiry must fit both unsigned long and nonnegative `time_t`; out-of-range is 0.
Eight added cases accept the maximum epoch, reject the next epoch and oversized
number/string lifetimes, then perform a default cache request to expose arithmetic
errors under UBSan. Scoped source review is CLEAN, including Makefile/metadata and
adapter; normal and sanitizer runs PASS. Public C API/response fields and SONAME 5 are unchanged.

The Linux/ARM64 harness links real parsing, refresh construction, account setters,
both handlers and IPC formatting/pipes. Issuer HTTP, loaded-account DB/encryption,
configuration and persistence notification are mocked; time/memory boundaries are
wrapped. It uses only synthetic payloads, not an issuer or daemon process.
Earlier [general Check suites](../validation/auth-resource-2026-10-14/linux/oidc-upstream-tests.log)
passed 143 checks; these are distinct from the final 131-check daemon-flow fixture.

ASan/UBSan used Clang **22.1.8**, Ubuntu **compiler-rt 22.1.2**, an explicit runtime
resource directory and original compiler builtin includes. Runtime options:
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`.
The final log has no sanitizer diagnostics. This is mixed-version runtime coverage;
the [initial missing-runtime link attempt](../validation/auth-resource-2026-10-14/linux/oidc-expiry-asan.log) is NOT PASS.

Fresh post-P2 native cycle: **PASS at the exact fork pin above**, with a different
fixture/run ID and user-confirmed real Device Flow enrollment exiting 0.
[Scope + audience acquisition](../validation/auth-resource-2026-10-14/linux/native-oct14/final-acquire-overrides.jsonl),
[renewal after actual expiry](../validation/auth-resource-2026-10-14/linux/native-oct14/final-refresh-overrides.jsonl),
[audience-only resume](../validation/auth-resource-2026-10-14/linux/native-oct14/final-resume-audience.jsonl),
and [restart/encrypted-account load/resume](../validation/auth-resource-2026-10-14/linux/native-oct14/final-restarted-resume.jsonl) PASS; load exited 0.
The exercised client's refresh counter was [14 before](../validation/auth-resource-2026-10-14/linux/native-oct14/final-counts-before-refresh.jsonl)
and [16 after](../validation/auth-resource-2026-10-14/linux/native-oct14/final-counts-after-refresh.jsonl).
`refresh_error` stayed **2 -> 2: zero new errors, not zero cumulative errors**;
the two errors predate this fresh fixture. After intentional account removal
(oidc-add exit 0), [unload control](../validation/auth-resource-2026-10-14/linux/native-oct14/final-after-unload.jsonl)
reported typed `setup_required` (JSON status `BLOCKED`, driver exit 3), as expected.
These runs do **not prove Keycloak honored resource/audience**. Controlled fixtures
prove outgoing overrides; the separate resource-server verifier enforces recipient policy.
Post-P2 daemon [rebuild](../validation/auth-resource-2026-10-14/linux/oidc-patched-build-final.log)/[install](../validation/auth-resource-2026-10-14/linux/oidc-patched-install-final.log) PASS;
installed Forge/package-consumer verification remains pending.
Separate Forge regression failures and repairs are recorded in the
[iteration report](../iterations/auth-api-resource-verification-v1.md#linux-regression-follow-up).
The final [Linux rerun](../validation/auth-resource-2026-10-14/linux/final-verified-regressions.log)
passes 11/11; original failed evidence remains available. These were not
oidc-agent native-cycle failures.

## Standalone Reproduction

Require Linux/ELF, C99 compiler (recorded: Clang 22.1.8), GNU Make/linker with
`--wrap` and `--gc-sections`, pkg-config, Check, libcurl development files and
libmicrohttpd headers; cJSON/list are bundled. Use an isolated pinned source copy
and a repository containing the baseline. No account files or real credentials.
The [fixture](../../vendor/oidc-agent/test/token_response_expiry/README.md) uses entry points present in both trees.

```sh
set -eu
: "${PATCHED_SOURCE:?Isolated source at 28d6f89701b64d5150b05d01f85b8a8eed54808e}"
: "${BASELINE_REPO:?Repository containing the baseline SHA}"
: "${CC:?Selected C compiler}"
WORK="$(mktemp -d)"
pkg-config --exists check libcurl libmicrohttpd
test "$(git -C "$PATCHED_SOURCE" rev-parse HEAD)" = 28d6f89701b64d5150b05d01f85b8a8eed54808e
test -z "$(git -C "$PATCHED_SOURCE" status --porcelain)"
mkdir -p "$WORK/pristine"
git -C "$BASELINE_REPO" archive f31513acaacaa7ebc115b3a329105d449b43ac99 \
  | tar -x -C "$WORK/pristine"
cp -R "$PATCHED_SOURCE/test/token_response_expiry" "$WORK/pristine/test/"
red_status=0
make -C "$WORK/pristine" -f test/token_response_expiry/Makefile -j4 \
  CC="$CC" ROOT="$WORK/pristine" BUILD_DIR="$WORK/red-build" test \
  > "$WORK/red.log" 2>&1 || red_status=$?
test "$red_status" -ne 0
grep 'Checks: 131, Failures: 106, Errors: 0' "$WORK/red.log"
make -C "$PATCHED_SOURCE" -f test/token_response_expiry/Makefile -j4 \
  CC="$CC" ROOT="$PATCHED_SOURCE" BUILD_DIR="$WORK/green-build" test \
  > "$WORK/green.log" 2>&1
grep 'Checks: 131, Failures: 0, Errors: 0' "$WORK/green.log"
```

Copy only the test directory to pristine production sources. Compilation/link
failure is not valid RED evidence. The fixture's version macro does not attest
daemon identity. These commands match the recorded target/flags with fresh output paths.

For the recorded sanitizer configuration, install Ubuntu compiler-rt and run:
```sh
ORIGINAL_BUILTIN_INCLUDE="$("$CC" -print-resource-dir)/include"
SYSTEM_RT=/usr/lib/llvm-22/lib/clang/22
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
make -C "$PATCHED_SOURCE" -f test/token_response_expiry/Makefile -j4 \
  CC="$CC" ROOT="$PATCHED_SOURCE" BUILD_DIR="$WORK/asan-build" \
  SANITIZE="-fsanitize=address,undefined -resource-dir=$SYSTEM_RT -isystem $ORIGINAL_BUILTIN_INCLUDE" \
  test > "$WORK/asan.log" 2>&1
grep 'Checks: 131, Failures: 0, Errors: 0' "$WORK/asan.log"
```
Require no sanitizer diagnostics as well as success. Commands build a focused
executable, not a deployed agent package; no private build prefix is required.

## Deployment And Submission

The patch pairs returned token/expiry, preserves the default cache for overrides,
and frees results once. Zero lifetime means expiry now; absent/invalid lifetime
means unknown (0). No JWT-based guess or cached/default lifetime is substituted.
Use real [Make/install metadata](../../vendor/oidc-agent/test/token_response_expiry/PACKAGING.md).
**The serving daemon must be patched/restarted**, including services, forwarded
sockets and existing C API remote routing. Client package discovery cannot attest it.
Preserve all scopes/audience/minimum-validity fields; no stripped-override, fabricated
expiry, raw-IPC or alternate-provider fallback. See the [adapter README](../../libraries/auth/oidc_agent/README.md).
Source P2 and Linux native gates are closed; installed Forge verification remains pending.
Forge records the pin in local `813735bc21e1030daaa57fa29d0918bd3bf7ba31`, not promoted to dev.
Submit only after explicit approval; fork publication is not an upstream submission.
Selected sanitized evidence is retained under `docs/validation`, outside generated
build trees. No account files, private keys or raw token values are retained there.
Retire the fork only after an official upstream pin passes equivalent regression,
package and live checks. This report remains **NOT_SUBMITTED**.

[baseline]: https://github.com/indigo-dc/oidc-agent/tree/f31513acaacaa7ebc115b3a329105d449b43ac99
[prerel]: https://github.com/indigo-dc/oidc-agent/tree/431dcd8a930e092a47de87b1782d24ff3727cae0
[pin]: https://github.com/SystasisIO/oidc-agent/commit/28d6f89701b64d5150b05d01f85b8a8eed54808e
[fix]: https://github.com/SystasisIO/oidc-agent/commit/d9661b3e61b933a8dd2ddc04bd4727322824330d
[parser]: https://github.com/indigo-dc/oidc-agent/blob/f31513acaacaa7ebc115b3a329105d449b43ac99/src/oidc-agent/oidc/flows/oidc.c#L99-L201
[handlers]: https://github.com/indigo-dc/oidc-agent/blob/f31513acaacaa7ebc115b3a329105d449b43ac99/src/oidc-agent/oidcd/oidcd_handler.c#L645-L750
