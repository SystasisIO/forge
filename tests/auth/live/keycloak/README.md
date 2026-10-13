# Isolated Keycloak Live Fixture

Disposable real issuer for [native live acceptance](../README.md) and
[resource-server acceptance](../../../integration/auth/README.md), not a
production configuration or completed gate. Only the coordinator runs the
commands below, manages certificate trust, containers and cleanup. No custom
OAuth/JWT engine, mocks, password grant, browser automation or master admin.

Project `forge-auth-live` publishes only `127.0.0.1:18443` over HTTPS. Its own
`forge-auth-live_data` volume holds the disposable H2 file database; no PostgreSQL,
external network, Gate configuration or existing volume is used. The image is
pinned to `26.8.0` and the supplied digest, verified against the public Quay
manifest; `pull_policy: never` requires the coordinator's cached image.

## Prepare Generated Inputs

Run from the Forge root, in a private interactive terminal with tracing off:

```sh
set +x
umask 077
export FORGE_AUTH_LIVE_DIR="$(pwd)/build-auth-resource/live-keycloak"
export ISSUER=https://localhost:18443/realms/forge-live
test -f "$FORGE_AUTH_LIVE_DIR/tls/server.crt"
test -f "$FORGE_AUTH_LIVE_DIR/tls/server.key"
```

Use the coordinator-generated one-day self-signed EC server leaf: `CA:FALSE`,
`serverAuth`, SAN `localhost`, PEM certificate and Keycloak-readable unencrypted
PEM private key. The user has explicitly approved temporary **localhost leaf**
trust in their macOS user Keychain. This approval is limited to this fixture:
no root CA, system trust,
TLS-verification bypass or production policy change. The coordinator owns trust
installation, verification and removal, including after failure/cancellation.
The fixture author performs no Docker or `security` execution.

Use a private Python **3.10+** virtual environment; the coordinator's Homebrew
Python 3.12 is suitable, macOS system Python 3.9 is not. Create the environment
with that interpreter if absent, then install the exact direct dependency pins
in [requirements.txt](requirements.txt). Do not install into system Python.
`VENV` below is a public path, not a secret; select the coordinator's existing
environment if it lives elsewhere:

```sh
VENV="$FORGE_AUTH_LIVE_DIR/venv"
# Only if absent: python3.12 -m venv "$VENV"
"$VENV/bin/python" -m pip install -r tests/auth/live/keycloak/requirements.txt
"$VENV/bin/python" -m pip check
"$VENV/bin/python" tests/auth/live/keycloak/resource_acceptance.py prepare \
  --directory "$FORGE_AUTH_LIVE_DIR"
```

The checked-in [realm template](realm.json) contains service-account role mapping,
but no passwords or client secrets. `prepare` generates random secrets for all
three confidential clients and a random disposable `fixture-user` password:

- `import/forge-live-realm.json`: generated import, including credentials.
- `resource-credentials.json`: fixture ownership marker and client secrets only.
- `login.txt`: two lines, `username: ...` and `password: ...`, for private human login only.

Files are written atomically as `0600` in `0700` directories; neither stdout nor
arguments/environment contain credentials. Human login needs only `login.txt`,
never the client-secrets file. Do not print, attach or collect any of these files.
Existing files, including an old native `login.txt`, are refused unless the
coordinator explicitly adds `--replace`. Replacement is for this owned fixture
only, after stopping its stand; it regenerates all credentials. Writes are
individually atomic, not a multi-file transaction: after interruption, rerun
with `--replace` and require `PASS/prepared` before starting a fresh owned volume.
Startup import does not update an existing realm.

Both Compose bind mounts remain read-only. The image runs as UID 1000 / GID 0;
host files may be UID 501, mode `0600`. The coordinator must verify read/search
access through Docker's bind mapping without world-readable permissions. Do not
change unrelated ownership, permissions, Gate config or volumes. TLS files are
neither generated nor modified by the harness; Compose is unchanged.

## Start And Discover (Coordinator Only)

Use a fresh fixture project/volume; stop if that name belongs to unrelated work.
Define this wrapper in the same private shell to avoid loading any `.env` file:

```sh
kc() {
  docker compose --env-file /dev/null --project-name forge-auth-live \
    -f tests/auth/live/keycloak/compose.yaml "$@"
}
kc config --quiet
kc up -d --pull never --no-build
```

Container-started is not ready. After startup, check public discovery with real
TLS verification and a bounded retry; use `pipefail` so a transport failure fails:

```sh
set -o pipefail
curl --fail --silent --show-error --ipv4 --noproxy localhost,127.0.0.1 \
  --cacert "$FORGE_AUTH_LIVE_DIR/tls/server.crt" \
  --connect-timeout 3 --max-time 10 --retry 20 --retry-delay 3 \
  --retry-connrefused --retry-max-time 120 \
  "$ISSUER/.well-known/openid-configuration" | \
  jq -e --arg issuer "$ISSUER" '
    .issuer == $issuer and
    .authorization_endpoint == ($issuer + "/protocol/openid-connect/auth") and
    .token_endpoint == ($issuer + "/protocol/openid-connect/token") and
    .device_authorization_endpoint == ($issuer + "/protocol/openid-connect/auth/device")
  ' >/dev/null
```

`forge-desktop` is public Authorization Code + PKCE (Proof Key for Code Exchange)
S256 + refresh; `forge-device` is public Device Flow + refresh. Use
`openid profile offline_access`, no client secret or audience override. Access
tokens last 30 seconds; online/offline sessions allow bounded refresh/restart
tests. Browser login is human-only with the private `login.txt`; the driver's
`live-<run-id>` is a separate local persistence account name.

Actual [Forge backend](../../../../libraries/auth/appauth/appauth_backend.mm)
calls `startHTTPListener:`; the pinned
[AppAuth handler](../../../../vendor/appauth/Sources/AppAuth/macOS/OIDRedirectHTTPHandler.m)
returns `http://127.0.0.1:<random-port>/`, or `http://[::1]:<random-port>/` on
IPv6-only systems. Keycloak 26.8.0's native loopback handling accepts these
portless registered URIs with arbitrary loopback ports. No redirect wildcard or
custom callback path is required.

Linux container `localhost` is not the macOS host. The coordinator's real
`socat` forwarder must listen on that container's `127.0.0.1:18443` and forward
to `host.docker.internal:18443`. Verify Docker Desktop actually routes the
loopback-published host port. Keep the issuer, HTTP Host and TLS SNI `localhost`;
do not substitute `host.docker.internal` in discovery or disable TLS verification.
Trust this same leaf only in the disposable Linux agent environment. Ensure IPv4
loopback works for the real agent, or supply a matching IPv6 loopback forwarder.
Forwarder setup/lifecycle belongs to the coordinator, not this Compose project.

## Real Resource Acceptance

This is a separate **workload token fixture, not user SSO** (single sign-on),
token exchange, or a substitute for native F4a. The boundary follows the
[accepted iteration](../../../../docs/iterations/auth-api-resource-verification-v1.md):
Keycloak issues tokens; the C++ driver exercises real `liboauth2` validation and
Forge HTTP/WS admission without weakening production verification.

- `forge-workload`: confidential service account, `client_credentials` only.
- `forge-resource`: confidential introspection client, no login/token grants.
- `forge-fixture-admin`: service account with only the owning realm's
  `realm-management/manage-realm` assignment and matching role scope. This
  permission is realm-wide, not key-only; never reuse it outside this fixture.

The workload uses Keycloak's built-in RFC 9068 header option (`at+jwt`), RS256,
the built-in audience mapper for `forge-resource`, the built-in user-session
note mapper for `client_id`, and the `profile` scope. Keycloak itself generates
`iat` and `jti`. Native desktop/device clients remain acquisition-only. No Java
extension, manual signing, password grant or protocol implementation is added.

After fresh import/discovery and coordinator build, run from an environment
where `localhost:18443` reaches this issuer and the driver is executable. Set
`RESOURCE_DRIVER` to the coordinator-built binary (public path only):

```sh
: "${RESOURCE_DRIVER:?Set the built resource driver path}"
"$VENV/bin/python" tests/auth/live/keycloak/resource_acceptance.py run \
  --directory "$FORGE_AUTH_LIVE_DIR" --driver "$RESOURCE_DRIVER"
```

The harness uses Authlib `OAuth2Session` for real client-credentials and
introspection requests, and `joserfc` for signature/claim verification against
real JWKS (JSON Web Key Set). TLS uses the supplied leaf, never `verify=False`;
redirects and environment proxy/credential inheritance are disabled. Before
mutating keys, it verifies the imported random ownership marker via Admin REST.
It then launches exactly one driver with `Popen`, private stdin/stdout pipes,
`--ca` as its only flag, and no secret environment variables or token files.

The [driver protocol](../../../integration/auth/README.md) is authoritative.
The first JSONL message contains `phase=initial`, `token` and
`introspection_client_secret`; after exactly `READY/rotation`, the harness adds
a real `rsa-generated` component with higher priority in this same owned realm.
It obtains a fresh token, cryptographically verifies it and proves its `kid`
was absent from the original JWKS while the original public key remains
unchanged and published. Only then does it send `phase=rotation` and the new
`token`. The same driver process and same warmed JWT verifier remain alive;
its long cache lifetime makes this unknown-key refresh, not scheduled expiry.

Only fixed PASS/READY/FAIL labels are emitted. `PASS/issuer-key-changed` and
`PASS/issuer-old-key-retained` establish actual rotation, not merely later token
expiry. `PASS/resource-acceptance` requires all exact driver labels and zero
exit status; partial PASS output is never acceptance. Overall timeout is 190 s,
with up to 2 s termination/reaping grace; timeout, SIGTERM or Ctrl-C cleans up
the child and fails. Raw child stderr is discarded and unexpected stdout is
rejected without echo. No tokens, subjects, key IDs, exceptions or HTTP bodies
are logged. Core dumps are disabled; Python allocations are not claimed to
provide total secret zeroization. Do not enable SDK/debug tracing.

Rotation never disables/removes the old signing key. A newly created component
may remain after a later failure; the coordinator removes it with the owned
disposable volume, not by modifying a shared realm. Run this gate serially,
without concurrent key changes. No script starts/stops Docker or changes trust.

HTTP and WS labels cover server-side admission and identity propagation. The
test WS peer uses standard Boost.Beast to send a real Authorization-bearing
Upgrade into Forge; it does not qualify Forge's outbound client header API.
This positive/rotation gate does not prove revocation, the full negative-test
matrix, native human login or product/WASM acceptance.

### Dependency And Issuer Qualification

Exact releases were checked against official package metadata/source; the
coordinator has reported successful installation in a private Python 3.12 venv.
This is dependency/API qualification, **not live resource acceptance**. The
script checks these exact installed versions before connecting. Transitive
dependencies are resolver-selected, not a fully hash-locked environment.

- [Authlib 1.8.0](https://pypi.org/project/Authlib/1.8.0/) requires Python 3.10+;
  [tagged requests integration](https://github.com/authlib/authlib/blob/v1.8.0/authlib/integrations/requests_client/oauth2_session.py)
  and [tagged OAuth client](https://github.com/authlib/authlib/blob/v1.8.0/authlib/oauth2/client.py)
  provide `fetch_token`, `introspect_token` and authenticated REST requests.
- [Requests 2.34.2](https://pypi.org/project/requests/2.34.2/) is the explicit
  HTTP dependency used by that integration.
- [joserfc 1.7.5](https://pypi.org/project/joserfc/1.7.5/), from the Authlib
  project, supplies [JWT verification and claims validation](https://jose.authlib.org/en/guide/jwt/)
  instead of deprecated `authlib.jose` or homemade JWT decoding.
- [Keycloak 26.2 release notes](https://www.keycloak.org/2025/04/keycloak-2620-released)
  document the built-in RFC 9068 access-token header. The pinned 26.8.0
  [OIDC config](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/protocol/oidc/OIDCConfigAttributes.java),
  [client-credentials grant](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/protocol/oidc/grants/ClientCredentialsGrantType.java),
  [audience mapper](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/protocol/oidc/mappers/AudienceProtocolMapper.java),
  [session-note mapper](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/protocol/oidc/mappers/UserSessionNoteMapper.java)
  and [RSA provider](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/keys/GeneratedRsaKeyProviderFactory.java)
  define the fixture's native settings; [Admin REST](https://www.keycloak.org/docs-api/26.8.0/rest-api/index.html)
  provides realm/component operations. No custom authenticator is required.

## Issuer-Side Refresh Evidence

Realm event storage explicitly includes `REFRESH_TOKEN`; the standard
`jboss-logging` listener logs successes at INFO via the runtime option in
Compose. No custom provider or admin login is needed. Raw event lines contain
session/user/token identifiers: never display, save, forward or `tee` them.
Only the filtered counts below may enter acceptance artifacts. Keep all SDK,
HTTP and Keycloak debug/trace logging off; Docker logs remain disposable private
runtime data and disappear with the fixture container.

In Bash with `pipefail`, define and call this aggregate before and after each
platform's driver `refresh` phase; do not run other logins/probes concurrently:

```sh
refresh_counts() {
  kc logs --no-color --no-log-prefix keycloak 2>&1 | awk '
    /\[org.keycloak.events\]/ && /realmName="forge-live",/ {
      client = ""
      if ($0 ~ /clientId="forge-desktop",/) client = "forge-desktop"
      if ($0 ~ /clientId="forge-device",/) client = "forge-device"
      if (client == "") next
      if ($0 ~ /type="CODE_TO_TOKEN",/) code[client]++
      if ($0 ~ /type="OAUTH2_DEVICE_CODE_TO_TOKEN",/) device[client]++
      if ($0 ~ /type="REFRESH_TOKEN",/ && /grant_type="refresh_token"/ &&
          $0 !~ /, error=/) refresh[client]++
      if ($0 ~ /type="REFRESH_TOKEN_ERROR",/) errors[client]++
    }
    END {
      split("forge-desktop forge-device", clients, " ")
      for (i = 1; i <= 2; i++) {
        c = clients[i]
        printf "{\"client\":\"%s\",\"code\":%d,\"device\":%d,\"refresh\":%d,\"refresh_error\":%d}\n", \
          c, code[c]+0, device[c]+0, refresh[c]+0, errors[c]+0
      }
    }'
}
refresh_counts
```

Require the appropriate initial grant count (`code` on macOS, `device` on Linux)
to be positive, the selected client's successful `refresh` count to increase
across the refresh phase, and no new `refresh_error`. These real issuer events
plus final driver PASS establish refresh-grant use, not just expiry advancement.
Zero/unavailable counts, container restart/log rotation or collector failure are
missing evidence, not PASS. Follow the parent's full human-login, separate-process
resume, refresh and invalidation/unload sequence; discovery alone is not F4a PASS.

## Cleanup (Coordinator Only)

After collecting only safe counts and driver status, invalidate the isolated
AppAuth item / unload and stop only the disposable Linux agent. Stop the owned
forwarder, remove only the approved temporary leaf trust, then run `kc down
--volumes` to remove this project's container, network and H2 volume. Remove the
generated import, credentials, `login.txt`, private venv and key/cert via the
coordinator's approved cleanup. Never touch
Gate resources. Startup import skips an existing realm: changed credentials or
realm settings require a fresh fixture volume, not a restart or shared-tenant edit.

References: [Keycloak containers](https://www.keycloak.org/server/containers),
[startup import](https://www.keycloak.org/server/importExport),
[26.8.0 loopback tests](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/test/java/org/keycloak/protocol/oidc/utils/RedirectUtilsTest.java),
[26.8.0 event listener](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/events/log/JBossLoggingEventListenerProvider.java).
