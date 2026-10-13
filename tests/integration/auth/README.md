# Real issuer resource driver protocol

`resource_driver.cpp` is a test-only service-account resource-server driver.
It does not qualify human login/SSO, revocation or product authorization.
The coordinator owns its CMake target, builds and live acceptance runs.

Link the owning targets: `forge_auth_liboauth2`, `forge_api_auth`,
`forge_api_http`, `forge_api_websocket`, `forge_net_http`,
`forge_net_websocket`, `forge_net_transport`, `forge_codec_json`, `forge_raw`,
`forge_asio`.

Invocation: `resource_driver --ca /absolute/path/to/ca.pem`. No secret flags,
environment inputs or token files are supported. The trusted issuer is fixed
to `https://localhost:18443/realms/forge-live`; audience and introspection
client are `forge-resource`, with required scope `profile`. The workload
token comes from the disposable `forge-workload` service account. JWT uses
RS256 and the production RFC 9068 profile, including protected `at+jwt`,
`client_id`, `iat` and `jti`. Introspection has a separate verifier and never
serves as a JWT fallback.

Private stdin consists of exactly two bounded JSONL records:

```json
{"phase":"initial","token":"PRIVATE","introspection_client_secret":"PRIVATE"}
{"phase":"rotation","token":"PRIVATE"}
```

Do not send the rotation record before `READY/rotation`. The harness must
rotate the actual Keycloak RSA signing-key component, retain the old public
key, obtain a new service-account token and prove that its `kid` changed and
matches the new published JWKS entry. The driver does not decode JWT headers
or implement JOSE. It preserves the exact same JWT verifier across phases.
After the initial scenarios, the driver waits 2 s on a bounded, cancellable
Asio timer before emitting READY, clearing the configured 1 s refresh
cooldown that starts at the first JWKS fetch. The harness needs no extra
sleep: rotate and issue the new token only after READY, then send it promptly.
Do not retry a rejected token or recreate the verifier to conceal a failure.
The JWKS snapshot lifetime is deliberately longer than the whole driver run,
so rotation must exercise unknown-key refresh rather than scheduled expiry.

Success stdout is exactly these newline-terminated, flushed labels:

```text
PASS/jwt-http
PASS/jwt-ws
PASS/introspection-http
PASS/introspection-ws
READY/rotation
PASS/rotation
PASS/shutdown
```

Each transport label requires a typed unary call and a server-stream open,
real verifier evidence, server-supplied subject replacement and stream end.
Only boolean acceptance assertions are returned on the resource wire, never
tokens, subjects or SDK diagnostics. `PASS/rotation` requires fresh HTTP and
WS admission after key rotation. `PASS/shutdown` is emitted only after server
stop, verifier drain and compute-pool shutdown. Failure emits one fixed
`FAIL/<stage>` label and exits nonzero; partial PASS output is not acceptance.

Input is bounded to 32 KiB per line, 16 KiB per token and 4 KiB for the
introspection secret. Initial input timeout is 30 s, rotation input timeout
90 s; transport scenarios have 10 s cancellation deadlines. A disposable
process watchdog imposes a 180 s hard limit; the harness should enforce 190 s
and must treat timeout/forced termination as failure, never successful drain.
Secret-owned driver buffers are wiped; parser and transport allocations are
not claimed to provide total zeroization. No exception text or parsed input
is printed. SDK/vendor stdout and stderr are discarded before private input
is read; only the driver can write to the result pipe. The hard deadline and
failure to join cancellation/cleanup force a nonzero process exit, without
claiming successful teardown. Production code is not modified by this task.

The WS test peer uses Boost.Beast for a real Authorization-bearing Upgrade,
as the existing F3 tests do. Forge owns guarded server admission, API binding,
frame DTOs, raw serialization and transport frame encode/decode. There is no
private protocol or fabricated verifier evidence. This proves server-side WS
admission and identity propagation, not Forge client token acquisition or an
authenticated Upgrade through the Forge client API. That client/handshake API
currently has no custom HTTP header option. Do not put bearer tokens into query
strings or substitute an in-memory token for handshake authentication.
