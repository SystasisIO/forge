# Forge Auth liboauth2

`forge_auth_liboauth2` implements `forge::auth::oauth2::token_verifier` using
the resource-server public API of the liboauth2 fork. Its sole public
module is `forge.auth.liboauth2.verifier`; vendor types remain private.
This implementation is pending the final F0 matrix and independent review,
not production acceptance. Legacy vendor defaults are not a supported profile.

## Configuration

Use one instance per immutable trusted issuer/resource/policy-version profile.
JWT uses configured HTTPS JWKS, an asymmetric algorithm allowlist and RFC 9068
protected `typ` (`at+jwt` or `application/at+jwt`). Opaque introspection is a
separate source with `client_secret_basic`; neither source falls back to the
other. Issuer/endpoints are explicit trusted configuration, never token hints.
Query, fragment and userinfo in these URLs are rejected. DNS/egress policy and
the authorization to configure private corporate endpoints belong to deployment.

```cpp
import forge.auth.liboauth2.verifier;

std::shared_ptr<forge::auth::liboauth2::verifier> make_verifier(
    forge::asio::compute::executor blocking) {
   using verifier = forge::auth::liboauth2::verifier;
   return verifier::create({
       .policy = {.issuer = "https://identity.example",
                  .audience = "resource-api", .version = "v1",
                  .algorithms = {forge::auth::oauth2::signature_algorithm::rs256},
                  .required_scopes = {"read"}},
       .source = verifier::jwks{.endpoint = "https://identity.example/keys"},
   }, std::move(blocking));
}
```

Create all instances on the application initialization thread before vendor
work starts. Pass that exact policy to `async_verify(secret_string, policy)`.
Resolve the returned issuer/subject through separately owned credential and
permission policy; this leaf does not create bindings, principals or sessions.
Default options are finite starting bounds, not a universal deployment policy.

TLS peer and hostname verification is always enabled. `ca_bundle` optionally
selects an administrator-owned absolute regular CA bundle file; otherwise
libcurl's configured default roots apply. No `verify=false`, proxy override,
discovery, redirects or retries are exposed. No environment is mutated.

## Verification And Bounds

The vendor performs signature/key selection, JWKS fetch and introspection.
CJOSE checks protected policy over the **same** immutable bytes accepted by the
vendor, not a reconstructed token. Jansson normalizes the returned verified
object; no second payload parser, JWT crypto or Forge key/cache engine is used.
Token-selected key URLs, embedded keys and critical header extensions are denied.

Issuer/audience/subject and strict integer `exp`/optional `nbf` are required.
JWT additionally requires `iat`, `client_id` and `jti`; opaque introspection
requires actual `active=true` and case-insensitive bearer `token_type`.
Wrong/missing types, NumericDate overflow, insufficient scopes, duplicate
JSON keys and signed payload NUL suffixes fail closed. No optional claims are
invented. Expiry is strict; clock skew applies to not-before/issued-at checks,
never extends `valid_until()` beyond the original expiry.
Offline JWT verification is not a revocation feed; a valid signed token may
remain usable until expiry. Binding/revocation freshness has a separate owner.

Input/response/approved-claim sizes, audiences, scopes, key count, admission,
HTTP timeout, provider refresh waits/cooldown and total caller deadline are
bounded. `verify.cache=none` is unconditional and not an option. Fork-owned
provider snapshots are isolated per instance and shared only by its config
clones, with bounded/coalesced unknown-key refresh; no persisted token cache.
An issuer outage is not HTTP 401 or `invalid_token`; size/capacity/cancellation
failures remain typed. Vendor messages are discarded, never forwarded to logs.
Forge-owned secret/config buffers are wiped; absolute zeroization of all
third-party allocations is not promised.

## Runtime And Tests

An externally owned `forge.asio.compute::executor` performs blocking work.
Admission uses `try_submit`, never an unbounded waiter queue. The full deadline
starts before submission; jobs without enough residual vendor budget never
perform an upstream call. Finite vendor budget is one HTTP timeout plus up to
two provider waits, with retries disabled. A structured Asio race waits on
cancellable completion/stop notifications, not the physical compute wait.
Caller cancellation/deadlines discard late results and cancel queued work;
they cannot physically preempt CJOSE/libcurl work.
Size bounds constrain CPU work but are not a hard CPU-time watchdog.

Call `request_stop()`, await `async_drain()`, then shut down the compute pool.
Admission tickets survive canceled callers until physical work has ended.
No adapter thread pool, polling worker or detached coroutine is created.
Drain cancellation does not mean successful teardown; dependency owners must
still await drain. The private process-wide vendor lifecycle is reference
counted; do not separately initialize/shut down liboauth2 in the same process.

`test_forge_auth_liboauth2` uses a real generated-CA HTTPS fixture, CJOSE-signed
tokens and the actual adapter for claims/purpose/isolation/cache-collision,
size/URL/TLS/outage, cancellation/backpressure/drain and bounded clone refresh.
This is a local fixture, not live IdP, Linux or installed-package acceptance.
The coordinator owns test execution, package consumers and final qualification.

Public dependencies: `forge_auth_oauth2`, `forge_asio`.
Private dependencies: `oauth2-internal`, `Boost::url`; the vendor owns private
CJOSE/Jansson/libcurl/OpenSSL dependency closure. The current pin is CJOSE
0.6.2.8; package/provenance verification belongs to the coordinator's F0 gate.
