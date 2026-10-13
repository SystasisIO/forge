# Isolated Keycloak F4a fixture

Disposable real issuer for [native live acceptance](../README.md), not a
production configuration or completed gate. Only the coordinator runs the
commands below, manages certificate trust, containers and cleanup. No OAuth
implementation, mocks, password grant, browser automation or admin credentials.

Project `forge-auth-live` publishes only `127.0.0.1:18443` over HTTPS. Its own
`forge-auth-live_data` volume holds the disposable H2 file database; no PostgreSQL,
external network, Gate configuration or existing volume is used. The image is
pinned to `26.8.0` and the supplied digest, verified against the public Quay
manifest; `pull_policy: never` requires the coordinator's cached image.

## Prepare generated inputs

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

The checked-in [realm template](realm.json) contains no users or passwords.
Generate the import with `jq` and Bash; the password is hidden terminal input,
never a CLI argument or environment variable, and stdout goes only to the
private generated file. Do not overwrite an existing acceptance run's import:

```sh
/bin/bash <<'SH'
set +x
set -euo pipefail
umask 077
: "${FORGE_AUTH_LIVE_DIR:?}"
mkdir -p "$FORGE_AUTH_LIVE_DIR/import"
set -o noclobber
trap 'unset fixture_password' EXIT
IFS= read -r -s -p 'Disposable fixture-user password: ' fixture_password </dev/tty
printf '\n' >/dev/tty
test -n "$fixture_password"
printf '%s' "$fixture_password" | jq -Rs \
  --slurpfile realm tests/auth/live/keycloak/realm.json '
  . as $password | $realm[0] | .users = [{
    username: "fixture-user", enabled: true,
    firstName: "Forge", lastName: "Fixture",
    email: "fixture-user@example.invalid", emailVerified: true,
    requiredActions: [], realmRoles: ["offline_access"],
    credentials: [{type: "password", value: $password, temporary: false}]
  }]' > "$FORGE_AUTH_LIVE_DIR/import/forge-live-realm.json"
SH
```

Generated import contains a plaintext disposable password: never commit, print
or collect it as an artifact. Both bind mounts are read-only. The image runs as
UID 1000 / GID 0; the coordinator must provide read/search permission through
Docker's bind mapping without making private inputs world-readable. Do not
change permissions or ownership of unrelated files.

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
tests. Browser login is human-only with `fixture-user`; the driver's
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
generated import/key/cert via the coordinator's approved cleanup. Never touch
Gate resources. Startup import skips an existing realm: changed credentials or
realm settings require a fresh fixture volume, not a restart or shared-tenant edit.

References: [Keycloak containers](https://www.keycloak.org/server/containers),
[startup import](https://www.keycloak.org/server/importExport),
[26.8.0 loopback tests](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/test/java/org/keycloak/protocol/oidc/utils/RedirectUtilsTest.java),
[26.8.0 event listener](https://github.com/keycloak/keycloak/blob/26.8.0/services/src/main/java/org/keycloak/events/log/JBossLoggingEventListenerProvider.java).
