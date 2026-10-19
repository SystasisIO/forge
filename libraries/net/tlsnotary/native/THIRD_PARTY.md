# Native dependency provenance

The Forge worker is API composition code. MPC, TLS, certificate verification,
transcript authentication, and selective disclosure are implemented by upstream
TLSNotary. No cryptography has been ported or substituted.

| Dependency | Exact source revision | License |
| --- | --- | --- |
| TLSNotary `tlsn` | `5f67389de3f8962227fddffd71d78a852acc9b01` | MIT OR Apache-2.0 |
| TLSNotary `mpz2` | `8f897838f4e24aff9da8e8b012e8ba35ae770141` | MIT OR Apache-2.0 |
| TLSNotary `tlsn-utils` | `e2f31e6a30bc5d1641ed6407fb1a38a595e523ce` | MIT OR Apache-2.0 |
| TLSNotary `rs-merkle` fork | `85f3e827451e18c21f110068b4322fb99d2f0a8c` | Apache-2.0 OR MIT (`Apache-2.0/MIT` upstream metadata) |

`Cargo.toml` fixes direct git revisions and `Cargo.lock` records every transitive
git revision, registry version, and checksum. The upstream basic MPC example
informs the sequence of `Session`, `commit`, TLS exchange, `prove`/`verify`, and
close calls in [session.rs](src/session.rs); upstream retains its own license
and copyright in Cargo's fetched source tree. The source tree under `donors/`
is never a build dependency.

The crate has no web/WASM target, `tlsn_insecure` is a compile error, and proxy
commit proposals are explicitly rejected by the verifier. The native release
binary is the only worker artifact installed by Forge.
