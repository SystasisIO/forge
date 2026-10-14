# Auth Resource Acceptance Evidence

These are selected, sanitized local and hosted acceptance artifacts from 2026-10-14. They
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
push alone was not promotion to `dev` or a Gate pin update; later promotion is recorded below.

## Hosted CI Acceptance

[Run 38508037293](https://github.com/SystasisIO/forge/actions/runs/38508037293),
[job 115669928164](https://github.com/SystasisIO/forge/actions/runs/38508037293/job/115669928164),
is SUCCESS at exact SHA `b279e367598dacb7dd90287a0d19b0dd8f7c39a8`.
Dependencies/catalog/regressions/packages/native-negative/evidence phases exit 0.
The Linux package gate is CLOSED/PASS: 24/24 regressions (53.42 s), two suites
repeated twenty times each (110.76 s), and 14/14 installed consumers.
The genuine installed-library negative control returns expected typed
`setup_required`, JSON `BLOCKED`, exit 3. It is not a new IdP login.

Artifact `11870068970` was downloaded with a matching ZIP SHA-256:
`b8d09602f9d02c358769904c949dc250502a3428e5da0f778bd1709ebc0b3ebb`.
Its five sanitized files are preserved unchanged outside generated builds:

| File | Evidence |
| --- | --- |
| [provenance.txt](ci-b279e367/provenance.txt) | Exact SHA, Ubuntu 26.04/libstdc++ profile and dependency pins |
| [phases.tsv](ci-b279e367/phases.tsv) | All six phase exit codes 0 |
| [regressions.txt](ci-b279e367/regressions.txt) | 24/24 tests and two twenty-repeat suites |
| [package-consumers.tsv](ci-b279e367/package-consumers.tsv) | 14/14 installed consumers PASS |
| [native-negative.json](ci-b279e367/native-negative.json) | Expected setup-required exit 3 through the genuine installed library |

Historical failed/cancelled runs and their fixes remain separate in the
[hosted verification section](../../iterations/auth-api-resource-verification-v1.md#hosted-linux-verification).
Human native PASS remains at `d5d29aac`; runtime-fix evidence remains at `b099d6aa`.
The successful integrated CI includes development revision `84619cd2` via merge
`a7c17ea6`; `9eac8072` is historical, not unchanged `origin/dev`.
Both reviewers returned FINAL CLEAN. Feature and `dev` were atomically promoted
to `08799dd6bd90b46d85bac3185459d100ca065be2`, with remote refs confirmed.
The build and temporary artifact ZIP were removed; these five safe files remain
committed. Docker was not started; removal of stopped `forge-auth-resource-linux`
alone is deferred while its API is off. Gate pin is next, not updated.
The exact successful CI remains `b279e367`; upstream reports remain NOT_SUBMITTED.
