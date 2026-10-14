# Forge Auth OIDC Agent

`forge_auth_oidc_agent` is a bounded asynchronous adapter for the public
`liboidc-agent5` C API requiring the maintained SystasisIO/oidc-agent fork profile
**`5.3.8-systasis.1`**, not stock `5.3.8`. It calls only
`getAgentTokenResponse`, `getAgentTokenResponseForIssuer` and
`secFreeAgentResponse`; no IPC protocol or refresh-token persistence is
reimplemented in Forge.

Use it on Linux when an application delegates account/refresh state to
oidc-agent. It acquires credentials; it is not a resource-server token verifier
or a guarantee that an issuer honored requested scope/resource policy.
Required fork source pin:
[28d6f89701b64d5150b05d01f85b8a8eed54808e](https://github.com/SystasisIO/oidc-agent/commit/28d6f89701b64d5150b05d01f85b8a8eed54808e).
The signed-time range fix is closed, scoped vendor/adapter review is CLEAN, and
the final normal/sanitizer fixture passes 131 checks. This is not a release
verdict: **Linux native PASS** covers the cycle below; the Linux installed
package gate is **CLOSED/PASS** at Forge `b279e367` in hosted run `38508037293`.
Final documentation closure, promotion and retained-build cleanup remain pending.

## Modules

- `forge.auth.oidc_agent.exceptions`: typed adapter and C-response failures.
- `forge.auth.oidc_agent.client`: account/issuer selection and the concrete
  OAuth2 token provider.

```cpp
import forge.asio.compute;
import forge.auth.oidc_agent.client;

auto blocking = forge::asio::compute::pool{{
    .worker_threads = 1,
    .max_pending_tasks = 0,
    .max_waiting_submissions = 32,
    .thread_name = "forge-oidc-agent",
}};
auto provider = forge::auth::oidc_agent::client::create(
    {
        .selection = {.kind = forge::auth::oidc_agent::selection_kind::issuer,
                      .value = "https://issuer.example"},
        .application_hint = "forge-desktop",
    },
    blocking.get_executor());
auto token = co_await provider->async_access_token({
    .scopes = {"openid", "profile", "email"},
    .audience = "https://resource.example",
    .minimum_validity = std::chrono::seconds{60},
});
```

Call from an application coroutine and use scope/audience values configured for
its real issuer and resource. Every requested scope is encoded into the public
C API's space-separated scope argument; audience and minimum validity are passed
without dropping overrides. Empty scope sets and absent audience mean agent
defaults, not a workaround for an override failure. Empty individual scopes,
an empty supplied audience, embedded NULs, and whitespace inside an individual
scope are rejected.

The returned daemon expiry must be positive, representable as a host timestamp,
not expired, and satisfy the requested minimum validity. Otherwise the adapter
raises typed `forge::auth::oidc_agent::exceptions::invalid_response` and releases
the C response. It does not retry with stripped scopes/audience, decode a JWT to
guess expiry, inherit a cached/default lifetime, use raw IPC, or switch providers.
Correct request encoding and token acquisition do not prove recipient binding;
the resource-server verifier must enforce the intended recipient separately.

## Required Client And Daemon

The leaf CMake requires `OIDCAgent::client`,
`OIDCAgent_VERSION=5.3.8-systasis.1`, public header `oidc-agent/api.h`, and the
real library's transitive dependencies. Root configuration discovers the real
pkg-config package on Linux. `FORGE_ENABLE_OIDC_AGENT=ON` fails without the exact
version; `AUTO` skips an unavailable backend, without selecting an older one.
Installed consumers require the matching dependency as well.

**The actual daemon serving the selected socket must also be patched.** Replacing
only liboidc-agent, its headers, or the Forge binary does not fix an already
running stock daemon: wrong expiry is generated inside its token handlers.
Restart the patched daemon and verify the explicitly selected socket. Agent
services, forwarded sockets, and any existing public-C-API remote routing must
also reach a patched daemon. Package version discovery alone does not attest
which daemon is running; do not claim this adapter performs such attestation.

Build/install the daemon and library from the same pinned source using the
[Make packaging instructions](../../../vendor/oidc-agent/test/token_response_expiry/PACKAGING.md).
`make install_lib-dev` installs genuine generated `liboidc-agent.pc` metadata;
do not fabricate a `.pc` file or override VERSION to bypass the check. Public
C API/response fields and Linux SONAME `liboidc-agent.so.5` are unchanged.
From the Forge root on Linux, select supported C/C++23 compilers and the actual
installed metadata directory, then configure the exact backend:

```sh
: "${OIDC_AGENT_PKGCONFIG_DIR:?Installed fork pkg-config directory}"
: "${FORGE_BUILD:?Forge build directory}"
: "${CC:?Supported C compiler}"
: "${CXX:?Supported C++23 compiler}"
export PKG_CONFIG_PATH="$OIDC_AGENT_PKGCONFIG_DIR${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --validate liboidc-agent
test "$(pkg-config --modversion liboidc-agent)" = 5.3.8-systasis.1
pkg-config --cflags --libs liboidc-agent
pkg-config --static --libs liboidc-agent
cmake -S . -B "$FORGE_BUILD" -G Ninja -DFORGE_ENABLE_OIDC_AGENT=ON \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
cmake --build "$FORGE_BUILD" --target forge_auth_oidc_agent -j4
```

Inspect the emitted header/library paths and static dependencies. Restart the
patched serving daemon separately and verify its selected socket; configuring
or linking this target does not manage daemon lifecycle.

## Execution And Secrets

The embedding application owns the bounded blocking pool and passes its
executor to every client that shares the same `oidc-agent` lane. Use one worker
because the public C API is blocking and exposes no hard-cancel operation.
Cancellation stops the caller's wait; a late result is securely released and
never delivered. Application shutdown must stop admission and coordinate agent
availability before joining the pool, because an in-flight external C call may
delay that join.
The private C response is freed with `secFreeAgentResponse` on every path; its
token is copied into a move-only OAuth2 `access_token` before that release.
The agent owns account configuration and refresh state. An agent error becomes
typed `forge::auth::oauth2::exceptions::setup_required`; its error/help text is
not logged or put into Forge exception context.
Never log token values, raw agent responses, vendor error/help strings, refresh
tokens, or account-file contents. Do not enable vendor debug tracing in production.

## Tests

The focused [adapter test source](../../../tests/auth/oidc_agent_tests.cpp) uses
the public oidc-agent C ABI with a deterministic test implementation. It covers
account and issuer selectors,
scope/audience/minimum-validity mapping, success/error response release,
malformed response rejection, typed setup-required errors, and cancellation
while the blocking C call remains in flight. Tokens never appear in test output.
It also exercises invalid/insufficient/unrepresentable expiry, one-call behavior
without fallback, and response release. Adapter unit fixtures remain distinct
from daemon-flow and installed-package tests.

Final evidence on 2026-10-14: the identical 131-check daemon-flow fixture is RED
on pristine baseline (106 failures, 0 errors) and GREEN on the frozen patch in
both normal and ASan/UBSan runs (0 failures/errors). Sanitizers use Clang 22.1.8
with Ubuntu compiler-rt 22.1.2, leak detection and halt-on-error enabled; no
sanitizer diagnostics were recorded. The initial missing-runtime link failure
is not PASS. Earlier general upstream Check targets passed 143 checks.

The fresh post-P2 live cycle at the exact fork pin above is **Linux native PASS**:
user-confirmed Device Flow enrollment exited 0; scope + audience acquisition and
renewal after actual expiry, audience-only resume, and daemon restart/encrypted
account load/resume passed. Refresh counts increased **14 -> 16** while cumulative
`refresh_error` stayed **2 -> 2: no new errors, not a zero total**. The two errors
predate this fresh fixture/run ID. After account removal (oidc-add exit 0), the
driver returned typed `setup_required` and exit 3 as expected, not a live failure.
This does **not** prove Keycloak applied resource/audience; controlled fixtures
prove outgoing overrides and the separate resource-server verifier owns recipient
enforcement.

**P2 signed-`time_t` range issue is CLOSED in source and final tests.** Expiry
outside unsigned-long/nonnegative-time_t range becomes unknown; added cases cover
the maximum epoch, the next epoch, oversized number/string lifetimes, and a
subsequent default cache request under UBSan. Scoped review includes the vendor
Makefile/metadata and adapter and is CLEAN.

Post-P2 daemon rebuild/install and the covered native cycle passed. Installed
Linux Forge/catalog package verification is **CLOSED/PASS** at exact SHA
`b279e367598dacb7dd90287a0d19b0dd8f7c39a8` in
[run 38508037293](https://github.com/SystasisIO/forge/actions/runs/38508037293):
24/24 regressions, two suites repeated twenty times each and 14/14 installed
consumers PASS. The separate genuine installed-library native-negative check
returned expected typed `setup_required`, exit 3; it is not a new IdP login
or proof of a running patched daemon. The recorded human native cycle remains
at `d5d29aac774eecd06a39371459cac5c5099c6c85`; runtime-fix evidence remains at
`b099d6aaca417c06bb4922abadf604cbbcfde638`. SOURCE/CI/PACKAGE review is CLEAN;
final documentation closure, promotion and retained-build cleanup remain pending. See the
[iteration record](../../../docs/iterations/auth-api-resource-verification-v1.md)
for the separate reruns and retained historical failures.
Logs/hashes, exact commit links and reproduction commands are in the
[upstream draft](../../../docs/donors/oidc-agent-upstream-report.md), which remains
**NOT_SUBMITTED**. Published fork commits are not an upstream issue/PR submission.
Selected sanitized evidence is preserved under `docs/validation`, outside
generated builds, including the five successful CI files in the
[evidence index](../../../docs/validation/auth-resource-2026-10-14/README.md#hosted-ci-acceptance).

This leaf is not an OIDC agent process manager, account-config generator,
browser flow, issuer validator or refresh-token store.
