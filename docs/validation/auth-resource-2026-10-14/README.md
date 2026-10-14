# Auth Resource Acceptance Evidence

These are selected, sanitized local acceptance artifacts from 2026-10-14. They
contain test results, timestamps and synthetic issuer counters, not tokens,
passwords, private keys, encrypted account files or browser callback URLs.
Generated build trees and disposable issuer state are not evidence archives.

The [iteration report](../../iterations/auth-api-resource-verification-v1.md)
records the source revisions, environment, outstanding gates and interpretation.
The [oidc-agent report](../../donors/oidc-agent-upstream-report.md) explains the
expected pristine RED result and the final patched normal/sanitizer results.

Native macOS and Linux logs cover acquisition, actual-expiry renewal, process
restart and local credential removal. They do not prove that the issuer honored
an audience override or that Gate has implemented a resource consumer. Resource
verification, client token acquisition and product authorization are distinct.

`linux/final-regressions.log` is retained failed evidence, not a passing run.
The two `websocket-observation-repeat.log` files record twenty successful repeats
on each platform after replacing fixed setup sleeps with bounded observation.
The source fix preserves pending-write, cancellation and drain assertions.

These local checks are separate from the subsequently authorized hosted CI.
No upstream bug report, release or tag was created by them; a feature-branch
push is not promotion to `dev` or a Gate pin update.

## Hosted CI Snapshot

Reported status on 2026-10-14: [run 38483751720](https://github.com/SystasisIO/forge/actions/runs/38483751720)
at `363423a83bb7f835e95baa0f40a03b3deb1b7d49` finished **FAILED**.
Dependencies, catalog and regressions PASS; packages FAILED at the third
consumer, `auth_oidc_agent`; the hosted native-negative phase was SKIPPED.
The fixture `.pc` placed absolute `stub.a` before `main.o`, so GNU ld could not
resolve `getAgentTokenResponse`, `getAgentTokenResponseForIssuer` and
`secFreeAgentResponse`. Production source is unchanged; a test CMake fixture
fix and fresh rerun are pending. Overall package/hosted acceptance is not PASS.

Preserve [run 38480451537](https://github.com/SystasisIO/forge/actions/runs/38480451537)
at `6fed3c48` as a dependency failure before Forge: Jansson 2.14.1's CMake
compatibility `< 3.5` failed with CMake 4, corrected in `a45747bd`.
[Run 38482513324](https://github.com/SystasisIO/forge/actions/runs/38482513324)
at `a45747bd` passed dependencies but failed catalog configuration because
dependency `LD_LIBRARY_PATH` shadowed system linker dependencies (`json_delete`);
`363423a8` corrects that setup. None of the three failed runs is recategorized as PASS.

Exact event SHAs and the phase boundaries are recorded in the
[hosted verification section](../../iterations/auth-api-resource-verification-v1.md#hosted-linux-verification).
The archived native PASS results remain separate from these hosted attempts.
Preserve sanitized hosted phase results here when available, without raw logs,
tokens or private configuration; no completed hosted/package PASS is claimed yet.
