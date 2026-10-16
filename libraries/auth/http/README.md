# Forge Auth HTTP

`forge_auth_http` owns product-neutral HTTP authentication evidence and
response policy. It adapts `forge_auth_session` records to strict Cookie,
Origin, CSRF and security-header mechanics, while `forge_net_http` remains the
sole Cookie and Set-Cookie parser/formatter owner.

Public modules:

- `forge.auth.http.bearer`: bounded Bearer token extraction from a native request
  or explicitly supplied Authorization header values.
- `forge.auth.http.exceptions`: typed evidence, Origin, CSRF, scope and policy
  failures.
- `forge.auth.http.types`: transient browser evidence and cookie/origin policy
  values.
- `forge.auth.http.policy`: extraction, authorization, cookie response helpers
  and browser security headers.

`extract_bearer_token(request)` returns a borrowed `std::optional<std::string_view>`;
an absent Authorization header returns `std::nullopt`. The explicit header-values
overload accepts `std::span<const std::string_view>`. Both reject duplicate headers,
unsupported schemes, an empty token, token whitespace/control bytes, and a token
larger than the selected limit (4096 bytes by default), using the existing typed
`duplicate_evidence`, `malformed_evidence` and `invalid_options` exceptions. The
Bearer scheme is ASCII case-insensitive and accepts one or more separating spaces.
The view is valid only while the request/header storage remains alive and unchanged;
consume it immediately or copy into `forge::crypto::core::secret_string` before
asynchronous ownership transfer. Do not log or persist the extracted token. This
helper verifies HTTP syntax only; digest lookup, expiry, token verification and
product permissions belong to the caller.

```cpp
import forge.auth.http.bearer;
import forge.crypto.core.secret_string;

auto token = forge::auth::http::extract_bearer_token(request, 4096);
if (token) {
   auto owned = forge::crypto::core::secret_string{std::string{*token}};
   // Pass owned to the application's existing credential verifier.
}
```

`extract_session_evidence` accepts explicitly supplied HTTP header values; it
does not own a router or transport facade. It requires one session cookie,
rejects duplicate Cookie, Origin and CSRF evidence, and requires CSRF evidence
for mutating methods. `authorize` validates a caller-loaded session and neutral
`forge::auth::credential::record`, exact Origin policy, a non-empty approved scope and double-submit
CSRF evidence. Safe `GET`, `HEAD` and `OPTIONS` requests may omit Origin;
mutating methods require it. Unknown methods and wildcard origins are rejected.
Origins are canonical serialized `http` or `https` origins: they have an
authority only, omit user info, path, query and fragment, and omit default
ports (`http:80`, `https:443`).

Cookie helpers always emit distinct `__Host-` names with `Secure`, `Path=/` and
`SameSite=Strict`; session cookies are `HttpOnly`, while CSRF and pre-session
cookies are frontend-readable. They use `append_set_cookie`, preserving one
`Set-Cookie` field per cookie. Security headers protect same-origin application
responses without applying `no-store` to assets unless the explicit sensitive
response option is selected.

This leaf has no persistence, routes, plugin, product-role, audit or rate-limit
dependency. Callers load records, choose allowed origins and integrate product
rate limiting/audit outside this library.

Authentication source expiry/freshness and durable identity bindings remain
caller-owned; these cookie helpers are not an OAuth callback or verifier.
No pairing dependency is required for configured or external-identity sessions.
See [Auth Credential](../credential/README.md) for the source-only migration;
cookie/Origin/CSRF behavior and session error categories remain unchanged.

Dependencies: `forge_auth_credential`, `forge_auth_session`, `forge_crypto_core`,
`forge_exceptions`, `forge_net_http` (private `Boost.URL`
parser backend).
