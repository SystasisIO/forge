# liboauth2 F0 Characterization

This is a standalone test fixture, not a production adapter or backend approval.
It targets OpenIDC liboauth2 2.4.0 commit
`85da4346435aec77f2ff11b529f2c1e8e5033c44`. The probe checks header/runtime
version 2.4.0; the builder must independently preserve exact commit provenance.
It includes public upstream headers only and calls `oauth2_token_verify()` with
supported `plain`, `jwks_uri` and `introspect` configuration methods.

## Build And Run

From the Forge root, after the independently built upstream/dependencies are
installed under `build-auth-f0/prefix`:

```sh
export PKG_CONFIG_PATH="$PWD/build-auth-f0/prefix/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/opt/pcre2/lib/pkgconfig"
cmake -S tests/auth/liboauth2_qualification -B build-auth-f0/probe -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_BUILD_RPATH="$PWD/build-auth-f0/prefix/lib"
cmake --build build-auth-f0/probe -j4
ctest --test-dir build-auth-f0/probe --output-on-failure -V
```

On Linux use the supported C++23 compiler and omit the macOS sysroot option.
Resolve any other dependency pkg-config directories from the selected build;
do not substitute another OpenSSL or unrecorded liboauth2 installation.
The runner never compiles anything. The direct probe CLI is
`probe <mode> <http://127.0.0.1:port/path>`; tokens/keys/secrets are internal
fabricated constants, not CLI arguments.

## Cases And Meaning

| Mode | Characterized public result / fixture observation |
|---|---|
| `invalid-jwt` | Local rejection of a deliberately invalid signature; baseline `false/401`. |
| `jwks-503` | Fresh process, no cached JWKS; endpoint returns HTTP 503 with empty keys. Baseline `false/401`. |
| `jwks-unreachable` | Fresh process, reserved non-listening loopback port. Baseline `false/401`. |
| `inactive` | HTTP 200, `active=false`. Baseline `false/401`. |
| `introspection-503` | Negative control: a real HTTP 503 survives as `false/503`. |
| `introspection-timeout` | Request consumed, no response headers sent. Baseline timeout collapses to `false/401`. |
| `redirect-307`, `redirect-308` | A different loopback port receives POST with the fabricated token and `client_secret_post` secret. |

The runner exits **0 only when these gaps are reproduced**, never because the
backend is approved. Exit 1 means the expected characterization differs; exit
2 means a harness/configuration/probe failure. Every summary explicitly emits
`backend_approved=false`. Do not treat this as the complete F0 negative matrix.
The JWKS vector is compact but has an invalid signature; these cases establish
key-fetch/error reporting behavior, not admission of a genuinely valid token
during an outage. Cached-success outage behavior is deliberately out of scope.

## Recorded Execution

The standalone `test_forge_auth_liboauth2_characterization` CTest run on
2026-10-13 completed in 23.12 seconds after the loopback reverse-DNS fix,
then 23.10 seconds after the diagnostic review fix.
All eight case observations matched the characterization above; the summary
reported `fixture_valid=true`, `gap_reproduced=true` and
`backend_approved=false`. The temporary CTest log is
`build-auth-f0/probe/Testing/Temporary/LastTest.log` from the Forge root;
generated build artifacts are removed after review. The enduring result record
and exact dependency manifest are in the
[qualification report](../../../docs/donors/auth-liboauth2-qualification.md).
This is a successful reproduction of backend gaps, not an F0 PASS. Linux,
HTTPS, protected-typ substitution and production integration remain untested
by this fixture. Future execution artifacts must be recorded independently.

All individual summary evidence flags require a valid measurement, not just
the expected return status. A coordinator-run negative harness check suppressed
the timeout request observation: the same probe results produced exit 2 and
all evidence flags false, as required. Both independent reviews are CLEAN only
for these test/evidence artifacts, not for the backend or Auth/API feature.

## Bounds And Secret Handling

- Each case runs in a fresh process; all payload/config objects are freed before
  process-global shutdown on normal completion. The init log has a discard sink;
  configuration, verification and config teardown receive NULL log handles.
- Public endpoint options request a 1-second timeout and zero retries. The exact
  candidate's resource-server paths ignore these options and use the 15-second
  HTTP default. A transfer timeout is not retried by this candidate. The timeout
  handler is held by an event, not a sleep loop.
- POSIX probe hard deadline is 20 seconds, child timeout at most 21 seconds,
  and runner work deadline 35 seconds, plus bounded server teardown. Forced
  termination is a harness error, not an authentication result or clean shutdown.
- Python suppresses HTTP/vendor/child logs and exception text, removes inherited
  proxy routing, caps request bodies at 8 KiB, and emits only case names,
  accepted/status values and booleans. It never replays captured child output.
- Servers bind only to 127.0.0.1 through `TCPServer.server_bind`, with a fixed
  numeric server name; startup does not call `HTTPServer`'s reverse-DNS lookup.
  HTTP is intentional: different ports prove the
  cross-origin 307/308 POST-body property; this does not qualify HTTPS, TLS,
  issuer trust, protected typ, production claims policy or revocation behavior.

Implementation and execution evidence are separate: the recorded run above
applies only to that standalone composition, not to unexecuted F0 gates.
