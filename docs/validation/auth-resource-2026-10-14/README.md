# Auth Resource Acceptance Evidence

These are selected, sanitized coordinator artifacts from 2026-10-14. They
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

No upstream bug report, CI run, release or tag was created by these checks.
